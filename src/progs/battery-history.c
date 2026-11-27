/*
 * battery-history: persistent battery history & handoff service (service_battery).
 *
 * Tracks physical battery packs by identity (vendor_model_serial) across
 * /sys/class/power_supply slots, detects hot-swaps on AC, cold-swaps across
 * hibernate/poweroff, and dual-battery discharge handoffs, re-applies saved
 * charge thresholds from rc.conf whenever a pack is inserted or swapped in,
 * runs opt-in ML adaptive charging (ChromiumOS ChargeHistory +
 * AdaptiveChargingController + TFLite inference, failing closed to manual or
 * saved baseline limits on every error), and records 7-day rolling curves in
 * /var/lib/locachy/battery/.
 *
 * Run "battery-history --once" to process transitions and record one snapshot.
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <inttypes.h>
#include <linux/netlink.h>
#include <math.h>
#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <poll.h>
#include <signal.h>
#include <sys/file.h>
#include <sys/signalfd.h>
#include <sys/socket.h>

#include "battery-controller.h"
#include "battery-history.h"
#include "battery-model-pins.h"
#include "battery-tflite.h"

#ifndef DEFAULT_RC_CONF
#define DEFAULT_RC_CONF "/etc/locachy/rc.conf"
#endif

#define BAT_FLUSH_INTERVAL_SEC 600   /* flush buffered .tsv samples every 10 min */
#define BAT_STATIC_REFRESH_SEC 1800  /* re-read static EC attrs every 30 min */
#define BAT_BUF_MAX 16               /* pending RAM samples per tracked pack */
#define BAT_POLL_ACTIVE_MS 60000     /* 60 s while charging/discharging */
#define BAT_POLL_SW_NEAR_CAP_MS 15000 /* 15 s only while SW-capped pack is actively charging near cap */
#define BAT_POLL_IDLE_AC_MS 300000   /* 5 min when AC online & all packs Full/Not charging */

static volatile sig_atomic_t stop_flag;
static volatile sig_atomic_t flush_flag;
static volatile sig_atomic_t reload_flag;

static void on_sig(int sig)
{
    if (sig == SIGUSR1) flush_flag = 1;
    else if (sig == SIGHUP) reload_flag = 1;
    else stop_flag = 1;
}

struct slot_state {
    char slot[48];
    int occupied;
    int ever_seen;
    struct bat_device dev;
    int64_t last_sample_ts;
    char last_state;
    int last_ac, last_ac_verified;
    int meta_written;
    struct bat_device saved_meta;
    struct bat_sample pending[BAT_BUF_MAX];
    int npending;
    int64_t last_flush_ts;
    struct bat_charge_slot_status charge_status;
    int sw_target_end; /* active SW charge_behaviour cap (20..99) on AC, else -1 */
};

static struct slot_state slots[BAT_MAX_DEVICES];
static int nslots;
static int any_prior_pack;
static char active_dis_slot[48];
static char active_dis_pack[96];
static int active_dis_dpct;

/* Daemon-only caches (only engaged by main()'s daemon loop; direct callers
 * such as test-battery-handoff.c invoking record_snapshot() get immediate
 * full scans & writes). */
static int daemon_mode;
/* Software charge_behaviour latching is allowed in daemon mode and in-process
 * tests, never in --once mode (which exits immediately without a service loop
 * to release inhibit-charge). */
static int sw_behaviour_allow_inhibit = 1;
static int sw_near_cap_poll;
static int64_t last_full_scan_ts;
static int last_ac_online;
static enum bat_system_ac last_system_ac;
static int all_packs_idle_ac;
static int64_t last_observation, last_monotonic, last_boot_time;

/* ---- Adaptive charging runtime (single controller + fail-closed model gate) ---- */

enum model_gate_state {
    MODEL_GATE_MISSING = 0,
    MODEL_GATE_INVALID,
    MODEL_GATE_READY,
};

struct bat_model_pin_spec {
    const char *tflite_name;
    size_t tflite_size;
    const char *tflite_sha512;
    const char *preproc_name;
    size_t preproc_size;
    const char *preproc_sha512;
};

static const struct bat_model_pin_spec prod_model_pins = {
    .tflite_name    = BAT_MODEL_TFLITE_NAME,
    .tflite_size    = BAT_MODEL_TFLITE_SIZE,
    .tflite_sha512  = BAT_MODEL_TFLITE_SHA512,
    .preproc_name   = BAT_MODEL_PREPROC_NAME,
    .preproc_size   = BAT_MODEL_PREPROC_SIZE,
    .preproc_sha512 = BAT_MODEL_PREPROC_SHA512,
};

static const struct bat_model_pin_spec *active_model_pins = &prod_model_pins;

/* Optional prediction override used only by in-process unit tests. */
static int (*bat_predict_override)(const struct bat_model *m,
                                   const struct bat_feature *feats,
                                   size_t n,
                                   double out[BAT_ADAPTIVE_OUTPUTS]);

static struct bat_model *adaptive_model;
static const struct bat_model_pin_spec *loaded_model_pins;
static struct bat_ctrl adaptive_ctrl;
static int adaptive_ctrl_ready;
static char adaptive_ctrl_pack_id[96];
static int adaptive_held_this_plug;
static int adaptive_released_this_plug;
static int adaptive_pred_failed;
static int alarm_wall_valid;
static double alarm_wall_boot;
static int64_t alarm_wall_ts;
static int64_t last_saved_hist[BAT_CTRL_SERIAL_MAX_I64];
static int last_saved_hist_len;

static int64_t current_epoch_ts(void)
{
    return bat_now_ts();
}

/* Boot-time seconds for the adaptive controller (monotonic, includes sleep).
 * Tests that inject LOCACHY_NOW_TS without LOCACHY_BOOT_SEC get a matching
 * synthetic boot clock so simulated time advances do not look like clock jumps;
 * tests that also set LOCACHY_BOOT_SEC control both axes independently. */
static double current_boot_sec(int64_t wall_ts)
{
    const char *bov = getenv("LOCACHY_BOOT_SEC");
    if (bov && bov[0]) {
        char *end = NULL;
        errno = 0;
        double v = strtod(bov, &end);
        if (!errno && end != bov && *end == '\0' && isfinite(v) && v > 0.0)
            return v;
    }
    const char *wov = getenv("LOCACHY_NOW_TS");
    if (wov && wov[0]) {
        double synth = (double)(wall_ts - 946684800LL) + 1.0;
        return synth > 1.0 ? synth : 1.0;
    }
    struct timespec bt;
    if (clock_gettime(CLOCK_BOOTTIME, &bt) == 0) {
        double v = (double)bt.tv_sec + (double)bt.tv_nsec / 1e9;
        return v > 1.0 ? v : 1.0;
    }
    return 1.0;
}

/* Convert a controller boot-time deadline to Unix epoch seconds for charge-status.
 * Returns 0 when unbounded (target_boot_sec < 0 or non-finite). */
static int64_t boot_to_wall_ts(double target_boot_sec, int64_t now_wall_ts, double now_boot_sec)
{
    if (!isfinite(target_boot_sec) || target_boot_sec < 0.0)
        return 0;
    const char *wov = getenv("LOCACHY_NOW_TS");
    const char *bov = getenv("LOCACHY_BOOT_SEC");
    if ((wov && wov[0]) || (bov && bov[0])) {
        double delta = target_boot_sec - now_boot_sec;
        if (!isfinite(delta)) return 0;
        int64_t ts = now_wall_ts + (int64_t)llround(delta);
        return ts > 0 ? ts : 0;
    }
    struct timespec rt, bt;
    if (clock_gettime(CLOCK_REALTIME, &rt) == 0 && clock_gettime(CLOCK_BOOTTIME, &bt) == 0) {
        double now_rt = (double)rt.tv_sec + (double)rt.tv_nsec / 1e9;
        double now_bt = (double)bt.tv_sec + (double)bt.tv_nsec / 1e9;
        int64_t ts = (int64_t)llround(now_rt + (target_boot_sec - now_bt));
        return ts > 0 ? ts : 0;
    }
    int64_t ts = now_wall_ts + (int64_t)llround(target_boot_sec - now_boot_sec);
    return ts > 0 ? ts : 0;
}

struct charge_config {
    int start, end, adaptive;
    int64_t full_until;
};

static struct charge_config charge_cfg = { .start = -1, .end = -1 };
static int64_t cancelled_full_until;
static int intent_loaded, intent_error;

static void read_charge_config(void)
{
    struct charge_config cfg = { .start = -1, .end = -1 };
    const char *rc_path = getenv("LOCACHY_RC_CONF");
    if (!rc_path || !rc_path[0]) rc_path = DEFAULT_RC_CONF;
    FILE *f = fopen(rc_path, "r");
    if (!f) {
        if (errno == ENOENT) charge_cfg = cfg;
        return;
    }
    char line[512];
    while (fgets(line, sizeof line, f)) {
        char *p = line;
        while (*p == ' ' || *p == '\t') p++;
        if (*p == '#' || *p == '\0') continue;
        char *eq = strchr(p, '=');
        if (!eq) continue;
        *eq++ = '\0';
        size_t kl = strlen(p);
        while (kl > 0 && (p[kl - 1] == ' ' || p[kl - 1] == '\t')) p[--kl] = '\0';
        while (*eq == ' ' || *eq == '\t') eq++;
        char *end;
        int quoted = *eq == '"';
        if (quoted) eq++;
        errno = 0;
        long long v = strtoll(eq, &end, 10);
        if (errno || end == eq || (quoted && *end++ != '"')) continue;
        while (*end == ' ' || *end == '\t') end++;
        if (*end != '\0' && *end != '\r' && *end != '\n' && *end != '#') continue;
        if (strcmp(p, "battery_charge_start") == 0 && v >= 0 && v <= 99) cfg.start = (int)v;
        else if (strcmp(p, "battery_charge_end") == 0 && v >= 20 && v <= 100) cfg.end = (int)v;
        else if (strcmp(p, "battery_charge_adaptive") == 0) cfg.adaptive = v == 1;
        else if (strcmp(p, "battery_charge_full_until") == 0 && v >= 0) cfg.full_until = v;
    }
    if (!ferror(f)) charge_cfg = cfg;
    fclose(f);
}

/* A baseline belongs to this pack in this slot, not to whichever pack is
 * inserted next. Keep it separate from historical .meta files, which are
 * rewritten for ordinary samples and are only keyed by pack.
 *
 * Line 4 format (backwards-compatible):
 *   Hardware threshold:  <start> <end> <start_attr|-> charge_control_end_threshold
 *   Software behaviour:  -1 -1 <saved_behaviour> charge_behaviour */
struct charge_baseline {
    int is_behaviour;
    int start, end;
    char start_attr[40], end_attr[40];
    char saved_behaviour[32];
};

static int charge_baseline_path(const struct bat_device *b, char *path, size_t cap)
{
    if (!bat_safe_name(b->name) || !bat_safe_name(b->pack_id)) return -1;
    int n = snprintf(path, cap, "%s/.charge-%s.%s", bat_history_dir(), b->name, b->pack_id);
    return n > 0 && (size_t)n < cap ? 0 : -1;
}

static int charge_identity_matches(const struct bat_device *b)
{
    const char *root = bat_supply_root();
    int ok;
    long long present = bat_read_ll(root, b->name, "present", &ok);
    if (ok && present != 1) return 0;
    char vendor[64] = "", model[64] = "", serial[64] = "";
    (void)bat_read_line(root, b->name, "manufacturer", vendor, sizeof vendor);
    (void)bat_read_line(root, b->name, "model_name", model, sizeof model);
    (void)bat_read_line(root, b->name, "serial_number", serial, sizeof serial);
    return strcmp(vendor, b->vendor) == 0 && strcmp(model, b->model) == 0 &&
           strcmp(serial, b->serial) == 0;
}

static int valid_saved_behaviour_token(const char *tok)
{
    if (!tok || !tok[0] || !isalnum((unsigned char)tok[0]) || strlen(tok) >= 32) return 0;
    for (const unsigned char *p = (const unsigned char *)tok; *p; p++) {
        if (!(isalnum(*p) || *p == '-' || *p == '_')) return 0;
    }
    return 1;
}

/* 0 = not saved, 1 = valid saved baseline, -1 = unreadable/conflicting. */
static int load_charge_baseline(const struct bat_device *b, const char *path, struct charge_baseline *saved)
{
    memset(saved, 0, sizeof *saved);
    FILE *f = fopen(path, "r");
    if (!f) return errno == ENOENT ? 0 : -1;
    char identity[3][66];
    int ok = 1;
    for (int i = 0; i < 3; i++) {
        if (!fgets(identity[i], sizeof identity[i], f) || !strchr(identity[i], '\n')) {
            ok = 0;
            break;
        }
        *strchr(identity[i], '\n') = '\0';
    }
    if (ok) {
        ok = strcmp(identity[0], b->vendor) == 0 && strcmp(identity[1], b->model) == 0 &&
             strcmp(identity[2], b->serial) == 0 &&
             fscanf(f, "%d %d %39s %39s", &saved->start, &saved->end,
                    saved->start_attr, saved->end_attr) == 4;
        if (ok) {
            if (strcmp(saved->end_attr, "charge_behaviour") == 0) {
                saved->is_behaviour = 1;
                ok = saved->start == -1 && saved->end == -1 &&
                     valid_saved_behaviour_token(saved->start_attr);
                if (ok)
                    snprintf(saved->saved_behaviour, sizeof saved->saved_behaviour,
                             "%.31s", saved->start_attr);
            } else {
                saved->is_behaviour = 0;
                ok = saved->start >= -1 && saved->start < saved->end &&
                     saved->end > 0 && saved->end <= 100 &&
                     strcmp(saved->end_attr, "charge_control_end_threshold") == 0 &&
                     strcmp(saved->start_attr, saved->start < 0 ? "-" : "charge_control_start_threshold") == 0;
            }
        }
    }
    fclose(f);
    return ok ? 1 : -1;
}

static int sync_history_dir(void)
{
    int fd = open(bat_history_dir(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (fd < 0) return -1;
    int rc = fsync(fd);
    close(fd);
    return rc;
}

static int save_charge_baseline(const struct bat_device *b, const char *path, int start, int end)
{
    char tmp[600];
    int n = snprintf(tmp, sizeof tmp, "%s.tmp.XXXXXX", path);
    if (n <= 0 || (size_t)n >= sizeof tmp) return -1;
    int fd = mkstemp(tmp);
    if (fd < 0) return -1;
    int ok = dprintf(fd, "%s\n%s\n%s\n%d %d %s %s\n", b->vendor, b->model, b->serial,
                     start, end, start < 0 ? "-" : b->start_attr, b->end_attr) > 0 && fsync(fd) == 0;
    if (close(fd) != 0) ok = 0;
    if (!ok || rename(tmp, path) != 0) {
        unlink(tmp);
        return -1;
    }
    return sync_history_dir();
}

static int save_charge_behaviour_baseline(const struct bat_device *b, const char *path,
                                          const char *saved_behaviour)
{
    if (!valid_saved_behaviour_token(saved_behaviour)) return -1;
    char tmp[600];
    int n = snprintf(tmp, sizeof tmp, "%s.tmp.XXXXXX", path);
    if (n <= 0 || (size_t)n >= sizeof tmp) return -1;
    int fd = mkstemp(tmp);
    if (fd < 0) return -1;
    int ok = dprintf(fd, "%s\n%s\n%s\n-1 -1 %s charge_behaviour\n",
                     b->vendor, b->model, b->serial, saved_behaviour) > 0 && fsync(fd) == 0;
    if (close(fd) != 0) ok = 0;
    if (!ok || rename(tmp, path) != 0) {
        unlink(tmp);
        return -1;
    }
    return sync_history_dir();
}

/* ---- Model verification gate (fail closed) ---- */

static int hex_to_sha512(const char *hex, unsigned char out[64])
{
    if (!hex || strlen(hex) != 128) return -1;
    for (int i = 0; i < 64; i++) {
        unsigned char hi, lo;
        char c0 = hex[i * 2], c1 = hex[i * 2 + 1];
        if (c0 >= '0' && c0 <= '9') hi = (unsigned char)(c0 - '0');
        else if (c0 >= 'a' && c0 <= 'f') hi = (unsigned char)(10 + c0 - 'a');
        else if (c0 >= 'A' && c0 <= 'F') hi = (unsigned char)(10 + c0 - 'A');
        else return -1;
        if (c1 >= '0' && c1 <= '9') lo = (unsigned char)(c1 - '0');
        else if (c1 >= 'a' && c1 <= 'f') lo = (unsigned char)(10 + c1 - 'a');
        else if (c1 >= 'A' && c1 <= 'F') lo = (unsigned char)(10 + c1 - 'A');
        else return -1;
        out[i] = (unsigned char)((hi << 4) | lo);
    }
    return 0;
}

static int sha512_matches_hex(const unsigned char *buf, size_t len, const char *expected_hex)
{
    unsigned char expected[64], actual[EVP_MAX_MD_SIZE];
    unsigned int md_len = 0;
    if (hex_to_sha512(expected_hex, expected) != 0) return 0;
    EVP_MD_CTX *ctx = EVP_MD_CTX_new();
    if (!ctx) return 0;
    int ok = EVP_DigestInit_ex(ctx, EVP_sha512(), NULL) == 1 &&
             EVP_DigestUpdate(ctx, buf, len) == 1 &&
             EVP_DigestFinal_ex(ctx, actual, &md_len) == 1 &&
             md_len == 64 &&
             CRYPTO_memcmp(actual, expected, 64) == 0;
    EVP_MD_CTX_free(ctx);
    return ok;
}

static enum model_gate_state read_verified_artifact(int dirfd, const char *name,
                                                    size_t expected_size,
                                                    const char *expected_sha512,
                                                    unsigned char **out_buf)
{
    *out_buf = NULL;
    if (!name || !name[0] || expected_size == 0)
        return MODEL_GATE_INVALID;
    /* O_NOFOLLOW refuses symlinks; O_NONBLOCK avoids blocking on a planted FIFO. */
    int fd = openat(dirfd, name, O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0)
        return errno == ENOENT ? MODEL_GATE_MISSING : MODEL_GATE_INVALID;

    struct stat st;
    if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode) || st.st_size < 0 ||
        (uintmax_t)st.st_size != (uintmax_t)expected_size) {
        close(fd);
        return MODEL_GATE_INVALID;
    }

    unsigned char *buf = malloc(expected_size);
    if (!buf) {
        close(fd);
        return MODEL_GATE_INVALID;
    }
    size_t got = 0;
    while (got < expected_size) {
        ssize_t r = read(fd, buf + got, expected_size - got);
        if (r < 0) {
            if (errno == EINTR) continue;
            free(buf);
            close(fd);
            return MODEL_GATE_INVALID;
        }
        if (r == 0) {
            free(buf);
            close(fd);
            return MODEL_GATE_INVALID;
        }
        got += (size_t)r;
    }
    unsigned char extra = 0;
    ssize_t tail;
    do {
        tail = read(fd, &extra, 1);
    } while (tail < 0 && errno == EINTR);
    close(fd);
    if (tail != 0) {
        free(buf);
        return MODEL_GATE_INVALID;
    }
    if (!sha512_matches_hex(buf, expected_size, expected_sha512)) {
        free(buf);
        return MODEL_GATE_INVALID;
    }
    *out_buf = buf;
    return MODEL_GATE_READY;
}

static void unload_adaptive_model(void)
{
    if (adaptive_model) {
        bat_model_free(adaptive_model);
        adaptive_model = NULL;
    }
    loaded_model_pins = NULL;
}

/* Verify both pinned artifacts in <history_dir>/model on startup, SIGHUP, and
 * each adaptive evaluation. Never downloads anything. Invalid takes precedence
 * over missing; any parse error in bat_model_load maps to MODEL_GATE_INVALID. */
static enum model_gate_state model_gate_check(int force_reload)
{
    const struct bat_model_pin_spec *pins = active_model_pins;
    char mdir[560];
    int n = snprintf(mdir, sizeof mdir, "%s/model", bat_history_dir());
    if (n <= 0 || (size_t)n >= sizeof mdir) {
        unload_adaptive_model();
        return MODEL_GATE_INVALID;
    }
    int dfd = open(mdir, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (dfd < 0) {
        unload_adaptive_model();
        return errno == ENOENT ? MODEL_GATE_MISSING : MODEL_GATE_INVALID;
    }

    unsigned char *tfl_buf = NULL, *pb_buf = NULL;
    enum model_gate_state r_tfl = read_verified_artifact(
        dfd, pins->tflite_name, pins->tflite_size, pins->tflite_sha512, &tfl_buf);
    enum model_gate_state r_pb = read_verified_artifact(
        dfd, pins->preproc_name, pins->preproc_size, pins->preproc_sha512, &pb_buf);
    close(dfd);

    if (r_tfl == MODEL_GATE_INVALID || r_pb == MODEL_GATE_INVALID) {
        free(tfl_buf);
        free(pb_buf);
        unload_adaptive_model();
        return MODEL_GATE_INVALID;
    }
    if (r_tfl == MODEL_GATE_MISSING || r_pb == MODEL_GATE_MISSING) {
        free(tfl_buf);
        free(pb_buf);
        unload_adaptive_model();
        return MODEL_GATE_MISSING;
    }

    if (force_reload || !adaptive_model || loaded_model_pins != pins) {
        struct bat_model *loaded = NULL;
        if (bat_model_load(&loaded, tfl_buf, pins->tflite_size,
                           pb_buf, pins->preproc_size) != 0) {
            free(tfl_buf);
            free(pb_buf);
            unload_adaptive_model();
            return MODEL_GATE_INVALID;
        }
        unload_adaptive_model();
        adaptive_model = loaded;
        loaded_model_pins = pins;
    }
    free(tfl_buf);
    free(pb_buf);
    return MODEL_GATE_READY;
}

/* ---- Controller history persistence (/var/lib/locachy/battery/charge-history-<pack_id>.bin) ---- */

static int ctrl_history_path(const char *pack_id, char *path, size_t cap)
{
    if (!bat_safe_name(pack_id)) return -1;
    int n = snprintf(path, cap, "%s/charge-history-%s.bin", bat_history_dir(), pack_id);
    return n > 0 && (size_t)n < cap ? 0 : -1;
}

static int save_ctrl_history(int force)
{
    /* Persisted only while adaptive charging is opted in: users who have not
     * opted in get no new files, and their curves and limits are unchanged. */
    if (!charge_cfg.adaptive || !adaptive_ctrl_ready || !bat_safe_name(adaptive_ctrl_pack_id))
        return 0;

    int64_t buf[BAT_CTRL_SERIAL_MAX_I64];
    int n = bat_ctrl_serialize_history(&adaptive_ctrl, buf, BAT_CTRL_SERIAL_MAX_I64);
    if (n <= 0)
        return -1;
    if (!force && n == last_saved_hist_len &&
        memcmp(buf, last_saved_hist, (size_t)n * sizeof(int64_t)) == 0)
        return 0;

    char path[560], tmp[600];
    if (ctrl_history_path(adaptive_ctrl_pack_id, path, sizeof path) != 0)
        return -1;
    int tn = snprintf(tmp, sizeof tmp, "%s/.charge-history-%s.bin.tmp.XXXXXX",
                      bat_history_dir(), adaptive_ctrl_pack_id);
    if (tn <= 0 || (size_t)tn >= sizeof tmp)
        return -1;

    (void)mkdir(bat_history_dir(), 0755);
    int fd = mkstemp(tmp);
    if (fd < 0)
        return -1;

    int ok = fchmod(fd, 0600) == 0;
    const unsigned char *p = (const unsigned char *)buf;
    size_t rem = (size_t)n * sizeof(int64_t);
    while (ok && rem > 0) {
        ssize_t w = write(fd, p, rem);
        if (w < 0) {
            if (errno == EINTR) continue;
            ok = 0;
            break;
        }
        p += (size_t)w;
        rem -= (size_t)w;
    }
    if (ok && fsync(fd) != 0) ok = 0;
    if (close(fd) != 0) ok = 0;
    if (!ok || rename(tmp, path) != 0) {
        unlink(tmp);
        fprintf(stderr, "battery-history: cannot save %s: %s\n", path, strerror(errno));
        return -1;
    }
    (void)sync_history_dir();
    memcpy(last_saved_hist, buf, (size_t)n * sizeof(int64_t));
    last_saved_hist_len = n;
    return 0;
}

/* Returns 1 if a valid persisted history was loaded into c, 0 if starting empty. */
static int load_ctrl_history(struct bat_ctrl *c, const char *pack_id,
                             const struct bat_ctrl_power *power,
                             int64_t wall_sec, double boot_sec)
{
    char path[560];
    if (ctrl_history_path(pack_id, path, sizeof path) != 0)
        return 0;

    int fd = open(path, O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) {
        if (errno != ENOENT) {
            fprintf(stderr, "battery-history: ignoring %s: %s\n", path, strerror(errno));
        }
        return 0;
    }

    struct stat st;
    if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode)) {
        fprintf(stderr, "battery-history: ignoring %s: not a regular file\n", path);
        close(fd);
        return 0;
    }
    const size_t max_bytes = BAT_CTRL_SERIAL_MAX_I64 * sizeof(int64_t);
    if (st.st_size <= 0 || (uintmax_t)st.st_size > (uintmax_t)max_bytes ||
        ((size_t)st.st_size % sizeof(int64_t)) != 0) {
        fprintf(stderr, "battery-history: ignoring %s: unexpected size %lld bytes\n",
                path, (long long)st.st_size);
        close(fd);
        return 0;
    }

    int64_t buf[BAT_CTRL_SERIAL_MAX_I64];
    size_t want = (size_t)st.st_size;
    unsigned char *dst = (unsigned char *)buf;
    size_t got = 0;
    while (got < want) {
        ssize_t r = read(fd, dst + got, want - got);
        if (r < 0) {
            if (errno == EINTR) continue;
            fprintf(stderr, "battery-history: ignoring %s: read error: %s\n", path, strerror(errno));
            close(fd);
            return 0;
        }
        if (r == 0) {
            fprintf(stderr, "battery-history: ignoring %s: truncated read\n", path);
            close(fd);
            return 0;
        }
        got += (size_t)r;
    }
    unsigned char extra = 0;
    ssize_t tail;
    do {
        tail = read(fd, &extra, 1);
    } while (tail < 0 && errno == EINTR);
    close(fd);
    if (tail != 0) {
        fprintf(stderr, "battery-history: ignoring %s: trailing bytes\n", path);
        return 0;
    }

    size_t n_elems = want / sizeof(int64_t);
    if (bat_ctrl_deserialize_history(c, buf, n_elems, power, wall_sec, boot_sec) != 0) {
        fprintf(stderr, "battery-history: ignoring %s: invalid charge history contents\n", path);
        return 0;
    }
    memcpy(last_saved_hist, buf, want);
    last_saved_hist_len = (int)n_elems;
    return 1;
}

static void reset_adaptive_controller(int save_before_reset)
{
    if (save_before_reset && adaptive_ctrl_ready)
        (void)save_ctrl_history(0);
    memset(&adaptive_ctrl, 0, sizeof adaptive_ctrl);
    adaptive_ctrl_ready = 0;
    adaptive_ctrl_pack_id[0] = '\0';
    adaptive_held_this_plug = 0;
    adaptive_released_this_plug = 0;
    adaptive_pred_failed = 0;
    alarm_wall_valid = 0;
    alarm_wall_boot = 0.0;
    alarm_wall_ts = 0;
    last_saved_hist_len = 0;
}

static void adaptive_cleanup(void)
{
    reset_adaptive_controller(0);
    unload_adaptive_model();
}

static struct bat_ctrl_power ctrl_power_from_device(const struct bat_device *b,
                                                    const struct bat_ac_info *ac)
{
    double pct = (double)b->capacity_decipct / 10.0;
    if (pct < 0.0) pct = 0.0;
    if (pct > 100.0) pct = 100.0;
    return (struct bat_ctrl_power){
        .ac_connected = (ac->system == BAT_AC_ONLINE),
        .battery_full = (strcmp(b->status, "Full") == 0),
        .battery_pct = pct,
        .display_battery_pct = pct,
    };
}

/* Ensure the single bat_ctrl instance is bound to pack b->pack_id.
 * Initializes with enabled=false so callers explicitly engage adaptive mode
 * via bat_ctrl_set_config once hardware support and the model gate both pass
 * (which also triggers initial evaluation when already plugged into AC). */
static void ensure_ctrl_for_pack(const struct bat_device *b,
                                 const struct bat_ctrl_power *pwr,
                                 int64_t wall_sec, double boot_sec)
{
    if (adaptive_ctrl_ready && strcmp(adaptive_ctrl_pack_id, b->pack_id) == 0)
        return;

    if (adaptive_ctrl_ready)
        reset_adaptive_controller(1);

    struct bat_ctrl_config cfg;
    bat_ctrl_default_config(&cfg);
    cfg.supported = true;
    cfg.enabled = false;
    (void)bat_ctrl_init(&adaptive_ctrl, &cfg);
    snprintf(adaptive_ctrl_pack_id, sizeof adaptive_ctrl_pack_id, "%s", b->pack_id);
    adaptive_ctrl_ready = 1;

    if (!load_ctrl_history(&adaptive_ctrl, b->pack_id, pwr, wall_sec, boot_sec)) {
        (void)bat_ctrl_on_power(&adaptive_ctrl, pwr, wall_sec, boot_sec);
    }
}

static void ctrl_disable_if_enabled(int supported, int64_t now_ts, double boot_sec);

/* Read panel brightness percentage from the standard /sys/class/backlight ABI
 * (read only). base/backlight.c's hidraw path is not linked into this daemon, so
 * a panel without a backlight class device reports 0. */
static int read_screen_brightness_pct(void)
{
    const char *dir_root = getenv("LOCACHY_BACKLIGHT_DIR");
    if (!dir_root || !dir_root[0]) dir_root = "/sys/class/backlight";
    DIR *d = opendir(dir_root);
    if (!d) return 0;
    int best_max = 0;
    char best_dir[320] = "";
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        if (e->d_name[0] == '.') continue;
        int ok = 0;
        long long mv = bat_read_ll(dir_root, e->d_name, "max_brightness", &ok);
        if (ok && mv > best_max && mv <= INT32_MAX) {
            best_max = (int)mv;
            snprintf(best_dir, sizeof best_dir, "%.255s", e->d_name);
        }
    }
    closedir(d);
    if (best_max <= 0 || !best_dir[0]) return 0;
    int ok = 0;
    long long cur = bat_read_ll(dir_root, best_dir, "brightness", &ok);
    if (!ok || cur < 0) return 0;
    if (cur > best_max) cur = best_max;
    return (int)((cur * 100 + best_max / 2) / best_max);
}

/* Build the 201 ChromiumOS RankerExample features, run inference, and deliver
 * the result to adaptive_ctrl. Any failure calls bat_ctrl_on_prediction(false)
 * (controller stop path -> normal/manual charging, never hold). */
static struct bat_ctrl_decision run_adaptive_prediction(int64_t wall_sec, double boot_sec)
{
    struct bat_ctrl_context ctx;
    bat_ctrl_default_context(&ctx);
    time_t tt = (time_t)wall_sec;
    struct tm tmv;
    if ((int64_t)tt == wall_sec && localtime_r(&tt, &tmv) != NULL) {
        ctx.local_hour = tmv.tm_hour;
        ctx.local_minute = tmv.tm_min;
        ctx.day_of_week = tmv.tm_wday;
        ctx.day_of_month = tmv.tm_mday;
    }
    ctx.mode = BAT_CTRL_MODE_LAPTOP;
    ctx.screen_brightness_pct = read_screen_brightness_pct();
    ctx.reason = adaptive_ctrl.prediction_reason;

    struct bat_feature feats[BAT_CTRL_FEATURE_COUNT];
    int nf = bat_ctrl_features(&adaptive_ctrl, &ctx, wall_sec, boot_sec,
                               feats, BAT_CTRL_FEATURE_COUNT);
    double probs[BAT_ADAPTIVE_OUTPUTS];
    bool ok = false;
    if (nf == BAT_CTRL_FEATURE_COUNT) {
        if (bat_predict_override) {
            ok = bat_predict_override(adaptive_model, feats, (size_t)nf, probs) == 0;
        } else if (adaptive_model) {
            ok = bat_model_predict(adaptive_model, feats, (size_t)nf, probs) == 0;
        }
    }

    struct bat_ctrl_decision d = bat_ctrl_on_prediction(
        &adaptive_ctrl, ok, ok ? probs : NULL, wall_sec, boot_sec);
    if (!ok) {
        adaptive_pred_failed = 1;
        adaptive_held_this_plug = 0;
        adaptive_released_this_plug = 0;
        fprintf(stderr, "battery-history: adaptive prediction failed (controller=%s); "
                        "falling back to manual/baseline limits\n",
                bat_ctrl_state_name(adaptive_ctrl.state));
    }
    return d;
}

/* The writer never edits rc.conf. A consumed request cannot re-arm after
 * replug, a daemon restart, or a clock step back through its expiry. */
static int64_t charge_full_until(const struct bat_ac_info *ac, int64_t now)
{
    if (!charge_cfg.full_until) return 0;
    char path[560];
    int n = snprintf(path, sizeof path, "%s/charge-cancelled", bat_history_dir());
    if (n <= 0 || (size_t)n >= sizeof path) {
        intent_error = 1;
        return 0;
    }
    if (!intent_loaded || intent_error) {
        FILE *f = fopen(path, "r");
        if (!f) {
            intent_error = errno != ENOENT;
        } else {
            long long until;
            intent_error = fscanf(f, "%lld", &until) != 1 || until < 0;
            if (!intent_error && until > cancelled_full_until) cancelled_full_until = until;
            fclose(f);
        }
        intent_loaded = 1;
    }
    int64_t until = charge_cfg.full_until;
    if (intent_error || until <= cancelled_full_until) return 0;
    if (until > now && until - now <= BAT_CHARGE_FULL_SECONDS && ac->system == BAT_AC_ONLINE)
        return until;
    if (until - now > BAT_CHARGE_FULL_SECONDS) return 0;

    char tmp[600];
    snprintf(tmp, sizeof tmp, "%s.tmp.XXXXXX", path);
    int fd = mkstemp(tmp);
    int ok = fd >= 0;
    if (ok) {
        ok = dprintf(fd, "%lld\n", (long long)until) > 0 && fsync(fd) == 0;
        if (close(fd) != 0) ok = 0;
        if (!ok || rename(tmp, path) != 0) {
            unlink(tmp);
            ok = 0;
        }
        if (ok) ok = sync_history_dir() == 0;
    }
    if (ok) cancelled_full_until = until;
    intent_error = !ok;
    return 0;
}

static int present_charge_attr(const struct bat_device *b, const char *attr)
{
    char path[512];
    int n = snprintf(path, sizeof path, "%s/%s/%s", bat_supply_root(), b->name, attr);
    struct stat st;
    if (n <= 0 || (size_t)n >= sizeof path) return -1;
    if (stat(path, &st) != 0) return errno == ENOENT ? 0 : -1;
    return S_ISREG(st.st_mode) ? 1 : -1;
}

/* Adaptive control only uses the kernel's standard power_supply ABI:
 * charge_control_end_threshold first (never falling back if present and
 * unreadable/malformed), then charge_behaviour (auto + inhibit-charge). */
static const char *charge_check_support(const struct bat_device *b)
{
    if (!b->serial[0] || !strcmp(b->serial, "0") || !strcasecmp(b->serial, "unknown"))
        return "unsupported";
    if (b->has_end_thresh) {
        if (strcmp(b->end_attr, "charge_control_end_threshold") != 0) {
            int present = present_charge_attr(b, "charge_control_end_threshold");
            return present == 0 ? "unsupported" : "error";
        }
        if (b->has_start_thresh && strcmp(b->start_attr, "charge_control_start_threshold") != 0)
            return "unsupported";
        if (!b->has_start_thresh && present_charge_attr(b, "charge_control_start_threshold") != 0)
            return "error";
        return NULL;
    }
    if (present_charge_attr(b, "charge_control_end_threshold") != 0)
        return "error";
    if (b->has_charge_behaviour)
        return NULL;
    int behav_present = present_charge_attr(b, "charge_behaviour");
    if (behav_present == 0) return "unsupported";
    if (behav_present < 0) return "error";
    enum bat_charge_behaviour bh = BAT_BEHAV_UNKNOWN;
    int rc = bat_read_charge_behaviour(b, &bh, NULL, 0);
    if (rc == 0) return NULL;
    return rc > 0 ? "unsupported" : "error";
}

static struct slot_state *get_slot(const char *slot_name, int create)
{
    for (int i = 0; i < nslots; i++) {
        if (strcmp(slots[i].slot, slot_name) == 0) return &slots[i];
    }
    if (!create || nslots >= BAT_MAX_DEVICES) return NULL;
    struct slot_state *s = &slots[nslots++];
    memset(s, 0, sizeof *s);
    snprintf(s->slot, sizeof s->slot, "%s", slot_name);
    s->last_ac = -1;
    return s;
}

/* Restore previously inserted packs per slot from .meta files so cold-swaps
 * across hibernate/reboot are detected on startup. */
static void load_previous_slots(void)
{
    DIR *d = opendir(bat_history_dir());
    if (!d) return;
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        size_t len = strlen(e->d_name);
        if (len <= 5 || strcmp(e->d_name + len - 5, ".meta") != 0) continue;
        char mpath[560];
        int n = snprintf(mpath, sizeof mpath, "%s/%s", bat_history_dir(), e->d_name);
        if (n <= 0 || (size_t)n >= sizeof mpath) continue;
        struct bat_device b;
        int ins = 0;
        if (bat_load_pack_meta(mpath, &b, &ins) != 0 || !b.name[0]) continue;
        any_prior_pack = 1;
        struct slot_state *sl = get_slot(b.name, 1);
        if (sl) sl->ever_seen = 1;
        if (ins && sl && (!sl->occupied || b.last_seen_ts >= sl->dev.last_seen_ts)) {
            sl->occupied = 1;
            sl->dev = b;
            sl->dev.is_offline_pack = 0;
        }
    }
    closedir(d);
}

static int open_uevent_socket(void)
{
    int fd = socket(AF_NETLINK, SOCK_DGRAM | SOCK_CLOEXEC | SOCK_NONBLOCK,
                    NETLINK_KOBJECT_UEVENT);
    if (fd < 0) return -1;
    int rcv = 256 * 1024;
    (void)setsockopt(fd, SOL_SOCKET, SO_RCVBUFFORCE, &rcv, sizeof rcv);
    (void)setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &rcv, sizeof rcv);
    struct sockaddr_nl sa;
    memset(&sa, 0, sizeof sa);
    sa.nl_family = AF_NETLINK;
    sa.nl_groups = 1;
    if (bind(fd, (struct sockaddr *)&sa, sizeof sa) != 0) {
        close(fd);
        return -1;
    }
    return fd;
}

static int drain_power_uevents(int fd)
{
    char buf[4096];
    int power = 0;
    for (;;) {
        ssize_t n = recv(fd, buf, sizeof buf - 1, 0);
        if (n < 0) {
            if (errno == EINTR) continue;
            if (errno == ENOBUFS) {
                power = 1;
                continue;
            }
            if (errno == EAGAIN || errno == EWOULDBLOCK) break;
            return -1;
        }
        if (n == 0) return -1;
        buf[n] = '\0';
        for (ssize_t i = 0; i < n; ) {
            const char *s = buf + i;
            if (strcmp(s, "SUBSYSTEM=power_supply") == 0) power = 1;
            i += (ssize_t)strlen(s) + 1;
        }
    }
    return power;
}

static int mwh_of(double wh)
{
    return wh > 0 ? (int)(wh * 1000.0 + 0.5) : -1;
}

static int mv_of(double v)
{
    return v > 0 ? (int)(v * 1000.0 + 0.5) : -1;
}

static int pack_meta_changed(const struct slot_state *sl, const struct bat_device *b)
{
    if (!sl->meta_written) return 1;
    const struct bat_device *m = &sl->saved_meta;
    return strcmp(m->pack_id, b->pack_id) != 0 ||
           strcmp(m->name, b->name) != 0 ||
           strcmp(m->vendor, b->vendor) != 0 ||
           strcmp(m->model, b->model) != 0 ||
           strcmp(m->serial, b->serial) != 0 ||
           strcmp(m->technology, b->technology) != 0 ||
           mwh_of(m->energy_full_wh) != mwh_of(b->energy_full_wh) ||
           mwh_of(m->energy_design_wh) != mwh_of(b->energy_design_wh) ||
           mv_of(m->voltage_design_v) != mv_of(b->voltage_design_v) ||
           m->cycles != b->cycles ||
           m->has_start_thresh != b->has_start_thresh ||
           m->has_end_thresh != b->has_end_thresh ||
           m->start_thresh != b->start_thresh ||
           m->end_thresh != b->end_thresh;
}

static void save_slot_meta(struct slot_state *sl, int inserted, int64_t ts)
{
    if (bat_save_pack_meta(&sl->dev, inserted, ts) == 0) {
        sl->meta_written = inserted ? 1 : 0;
        sl->saved_meta = sl->dev;
    }
}

static void flush_slot_samples(struct slot_state *sl, int64_t now_ts)
{
    if (sl->npending <= 0) return;
    if (bat_append_samples_batch(sl->dev.pack_id, sl->pending, sl->npending) == 0) {
        sl->npending = 0;
        sl->last_flush_ts = now_ts;
    }
}

static void flush_all_pending(int save_meta, int64_t now_ts)
{
    for (int j = 0; j < nslots; j++) {
        struct slot_state *sl = &slots[j];
        if (!sl->occupied) continue;
        flush_slot_samples(sl, now_ts);
        if (save_meta && now_ts >= 946684800LL) {
            save_slot_meta(sl, 1, sl->dev.last_seen_ts > 0 ? sl->dev.last_seen_ts : now_ts);
        }
    }
    (void)save_ctrl_history(0);
}

static void queue_slot_sample(struct slot_state *sl, const struct bat_sample *s, int immediate)
{
    if (!daemon_mode || immediate) {
        flush_slot_samples(sl, s->ts);
        if (bat_append_sample(sl->dev.pack_id, s) == 0) {
            sl->last_sample_ts = s->ts;
            sl->last_state = s->state;
            sl->last_ac = s->ac;
            sl->last_ac_verified = s->ac_verified;
            sl->last_flush_ts = s->ts;
        }
        return;
    }
    if (sl->npending >= BAT_BUF_MAX) {
        flush_slot_samples(sl, s->ts);
    }
    if (sl->npending < BAT_BUF_MAX) {
        sl->pending[sl->npending++] = *s;
        sl->last_sample_ts = s->ts;
        sl->last_state = s->state;
        sl->last_ac = s->ac;
        sl->last_ac_verified = s->ac_verified;
        if (sl->last_flush_ts == 0) sl->last_flush_ts = s->ts;
        if (sl->npending >= BAT_BUF_MAX || s->ts - sl->last_flush_ts >= BAT_FLUSH_INTERVAL_SEC) {
            flush_slot_samples(sl, s->ts);
        }
    } else if (bat_append_sample(sl->dev.pack_id, s) == 0) {
        sl->last_sample_ts = s->ts;
        sl->last_state = s->state;
        sl->last_ac = s->ac;
        sl->last_ac_verified = s->ac_verified;
        sl->last_flush_ts = s->ts;
    }
}

static void clear_charge_status(void)
{
    char path[560];
    int n = snprintf(path, sizeof path, "%s/charge-status", bat_history_dir());
    if (n > 0 && (size_t)n < sizeof path) (void)unlink(path);
}

static void publish_charge_status(const struct bat_charge_status *status)
{
    struct bat_charge_status previous;
    if (bat_read_charge_status(&previous) == 0 && previous.adaptive == status->adaptive &&
        previous.full_until == status->full_until && previous.nslots == status->nslots) {
        int same = 1;
        for (int i = 0; i < status->nslots; i++) {
            const struct bat_charge_slot_status *a = &previous.slots[i], *b = &status->slots[i];
            if (strcmp(a->slot, b->slot) || strcmp(a->pack_id, b->pack_id) || strcmp(a->state, b->state) ||
                a->requested_end != b->requested_end || a->actual_end != b->actual_end ||
                a->release_ts != b->release_ts) {
                same = 0;
                break;
            }
        }
        if (same) return;
    }
    char path[560], tmp[600];
    int n = snprintf(path, sizeof path, "%s/charge-status", bat_history_dir());
    if (n <= 0 || (size_t)n >= sizeof path) return;
    snprintf(tmp, sizeof tmp, "%s.tmp.XXXXXX", path);
    int fd = mkstemp(tmp);
    if (fd < 0) return;
    int ok = fchmod(fd, 0644) == 0 && dprintf(fd, "%d %lld %d\n", status->adaptive,
                                             (long long)status->full_until, status->nslots) > 0;
    for (int i = 0; ok && i < status->nslots; i++) {
        const struct bat_charge_slot_status *s = &status->slots[i];
        ok = dprintf(fd, "%s %s %s %d %d %lld\n", s->slot, s->pack_id, s->state,
                     s->requested_end, s->actual_end, (long long)s->release_ts) > 0;
    }
    if (close(fd) != 0) ok = 0;
    if (!ok || rename(tmp, path) != 0) unlink(tmp);
}

static void charge_sample(const struct bat_device *b, const struct bat_ac_info *ac,
                          int64_t now, struct bat_sample *s)
{
    bat_sample_from_device(b, ac->system == BAT_AC_UNKNOWN ? ac->online :
                           ac->system == BAT_AC_ONLINE, now, s);
    s->ac_verified = ac->system != BAT_AC_UNKNOWN;
}

/* Disable adaptive charging in the controller if it is currently enabled. */
static void ctrl_disable_if_enabled(int supported, int64_t now_ts, double boot_sec)
{
    if (!adaptive_ctrl_ready) return;
    if (adaptive_ctrl.cfg.enabled || adaptive_ctrl.cfg.supported != (supported != 0)) {
        struct bat_ctrl_config cfg = adaptive_ctrl.cfg;
        cfg.supported = (supported != 0);
        cfg.enabled = false;
        (void)bat_ctrl_set_config(&adaptive_ctrl, &cfg, now_ts, boot_sec);
    }
    adaptive_held_this_plug = 0;
    adaptive_released_this_plug = 0;
    adaptive_pred_failed = 0;
}

/* Apply a managed threshold pair (HOLD, TOP-UP, or FULL) through the existing
 * EC-safe writer, saving the pack's pre-managed baseline on first write.
 * Returns 0 on success, -1 on readback/identity/baseline/write failure. */
static int apply_managed_thresholds(struct bat_device *b, int have,
                                    const struct charge_baseline *saved,
                                    const char *baseline_path,
                                    int start, int end,
                                    struct bat_charge_slot_status *s)
{
    s->requested_end = end;
    if (have && saved->is_behaviour)
        return -1;
    if (!b->has_start_thresh) start = -1;
    if ((b->has_start_thresh && bat_read_threshold(b, b->start_attr, 0, &b->start_thresh) != 0) ||
        bat_read_threshold(b, b->end_attr, 1, &b->end_thresh) != 0 ||
        (b->has_start_thresh && b->start_thresh >= b->end_thresh))
        return -1;
    if ((b->has_start_thresh && b->start_thresh != start) || b->end_thresh != end) {
        if (!charge_identity_matches(b) ||
            (have && (strcmp(saved->end_attr, b->end_attr) ||
                      strcmp(saved->start_attr, b->has_start_thresh ? b->start_attr : "-"))) ||
            (!have && save_charge_baseline(b, baseline_path,
                                           b->has_start_thresh ? b->start_thresh : -1,
                                           b->end_thresh) != 0) ||
            bat_apply_thresholds_to(b, start, end) != 0)
            return -1;
    }
    return 0;
}

/* Release software charge_behaviour back to "auto" and unlink our .charge-*
 * baseline marker. When force_auto is 0, only an active inhibit-charge (e.g.
 * orphaned after a crash) or an owned baseline triggers a write to "auto". */
static int sw_release_behaviour(struct bat_device *b, int have,
                                const struct charge_baseline *saved,
                                const char *baseline_path, int force_auto)
{
    if (have && !saved->is_behaviour)
        return -1;
    enum bat_charge_behaviour cur = BAT_BEHAV_UNKNOWN;
    char active[32] = "";
    if (bat_read_charge_behaviour(b, &cur, active, sizeof active) != 0) {
        b->has_charge_behaviour = 0;
        b->charge_behaviour = BAT_BEHAV_UNKNOWN;
        b->charge_behaviour_active[0] = '\0';
        return -1;
    }
    b->has_charge_behaviour = 1;
    b->charge_behaviour = cur;
    snprintf(b->charge_behaviour_active, sizeof b->charge_behaviour_active, "%s", active);

    /* Releasing an inhibit is always safe, so it is written before any identity
     * check; the baseline (keyed to this pack) is only removed for the same pack. */
    int need_auto = (cur == BAT_BEHAV_INHIBIT) || (cur != BAT_BEHAV_AUTO && (have || force_auto));
    if (need_auto && bat_write_charge_behaviour(b, BAT_BEHAV_AUTO) != 0)
        return -1;
    if (have) {
        if (!charge_identity_matches(b))
            return -1;
        if (unlink(baseline_path) != 0 && errno != ENOENT)
            return -1;
        if (sync_history_dir() != 0)
            return -1;
    }
    return 0;
}

/* Software hysteresis enforcer for BAT_CTL_SW_BEHAVIOUR:
 *   pct >= end         -> inhibit-charge (recording .charge-* baseline first)
 *   pct <= start       -> auto (unlinking .charge-*)
 *   start < pct < end  -> preserve current auto / inhibit-charge (no chatter)
 * Never latches inhibit-charge when off AC, in --once mode, or at end >= 100. */
static int sw_enforce_cap(struct bat_device *b, const struct bat_ac_info *ac,
                          int have, const struct charge_baseline *saved,
                          const char *baseline_path, int start, int end,
                          int *actual_end_out)
{
    *actual_end_out = -1;
    if (have && !saved->is_behaviour)
        return -1;
    if (end < 20 || end > 100)
        return -1;

    if (ac->system != BAT_AC_ONLINE || !sw_behaviour_allow_inhibit || end >= 100) {
        if (sw_release_behaviour(b, have, saved, baseline_path, end >= 100) != 0)
            return -1;
        if (end >= 100 && ac->system == BAT_AC_ONLINE)
            *actual_end_out = 100;
        return 0;
    }

    if (start < 0) start = end >= 5 ? end - 5 : 0;
    if (start >= end) start = end - 1;

    enum bat_charge_behaviour cur = BAT_BEHAV_UNKNOWN;
    char active[32] = "";
    if (bat_read_charge_behaviour(b, &cur, active, sizeof active) != 0) {
        b->has_charge_behaviour = 0;
        b->charge_behaviour = BAT_BEHAV_UNKNOWN;
        b->charge_behaviour_active[0] = '\0';
        return -1;
    }
    b->has_charge_behaviour = 1;
    b->charge_behaviour = cur;
    snprintf(b->charge_behaviour_active, sizeof b->charge_behaviour_active, "%s", active);

    int pct = b->capacity_pct;
    /* The baseline records what firmware reported before LOCachy took control. */
    const char *pre = cur == BAT_BEHAV_INHIBIT ? "auto" : active;
    if (pct >= end) {
        if (cur != BAT_BEHAV_INHIBIT) {
            if (!charge_identity_matches(b))
                return -1;
            if (!have && save_charge_behaviour_baseline(b, baseline_path, pre) != 0)
                return -1;
            if (bat_write_charge_behaviour(b, BAT_BEHAV_INHIBIT) != 0)
                return -1;
        } else if (!have) {
            if (!charge_identity_matches(b) ||
                save_charge_behaviour_baseline(b, baseline_path, "auto") != 0)
                return -1;
        }
    } else if (pct <= start) {
        if (sw_release_behaviour(b, have, saved, baseline_path, 1) != 0)
            return -1;
    } else {
        if (cur == BAT_BEHAV_INHIBIT) {
            if (!have) {
                if (!charge_identity_matches(b) ||
                    save_charge_behaviour_baseline(b, baseline_path, "auto") != 0)
                    return -1;
            }
        } else if (cur == BAT_BEHAV_AUTO) {
            /* Already auto: nothing to write; a baseline, if any, stays for the next cap. */
        } else {
            if (sw_release_behaviour(b, have, saved, baseline_path, 1) != 0)
                return -1;
        }
    }
    *actual_end_out = end;
    return 0;
}

/* Fail-safe release for every occupied BAT_CTL_SW_BEHAVIOUR slot before sleep,
 * hibernate, or service exit so the EC is never left inhibiting charge. */
static void release_all_sw_behaviour(void)
{
    for (int j = 0; j < nslots; j++) {
        struct slot_state *sl = &slots[j];
        if (!sl->occupied) continue;
        struct bat_device *b = &sl->dev;
        if (bat_charge_control_kind(b) != BAT_CTL_SW_BEHAVIOUR) continue;
        if (present_charge_attr(b, "charge_control_end_threshold") != 0) continue;
        char path[560];
        struct charge_baseline saved = { 0 };
        int have = charge_baseline_path(b, path, sizeof path) == 0
                   ? load_charge_baseline(b, path, &saved) : -1;
        if (have < 0 || (have && !saved.is_behaviour)) have = 0;
        (void)sw_release_behaviour(b, have, &saved, path, have);
        sl->sw_target_end = -1;
    }
}

/* Convert charge_alarm_at to a stable Unix timestamp, caching across routine
 * polls so sub-second realtime/boottime skew does not rewrite charge-status. */
static int64_t stable_release_ts(double charge_alarm_at, int64_t now_wall, double now_boot,
                                 int clock_stepped)
{
    if (!isfinite(charge_alarm_at) || charge_alarm_at < 0.0) {
        alarm_wall_valid = 0;
        return 0;
    }
    if (!alarm_wall_valid || clock_stepped || fabs(charge_alarm_at - alarm_wall_boot) > 0.5) {
        alarm_wall_boot = charge_alarm_at;
        alarm_wall_ts = boot_to_wall_ts(charge_alarm_at, now_wall, now_boot);
        alarm_wall_valid = 1;
    }
    return alarm_wall_ts;
}

/*
 * Status string mapping from controller state + daemon gate -> charge-status:
 *   adaptive=0, no full_until                  -> "manual"
 *   baseline/sysfs/write failure               -> "error"
 *   multi-pack or lacking standard ABI/serial  -> "unsupported" (BAT_CTRL_STATE_NOT_SUPPORTED)
 *   full_until > 0 on AC or USER_CANCELED      -> "full"        (BAT_CTRL_STATE_USER_CANCELED)
 *   adaptive=1, model file(s) missing          -> "model-missing"
 *   adaptive=1, bad size/hash/symlink/parse    -> "model-invalid"
 *   adaptive=1, AC offline                     -> "on-battery"
 *   adaptive=1, AC online, HEURISTIC_DISABLED  -> "learning"    (BAT_CTRL_STATE_HEURISTIC_DISABLED)
 *   adaptive=1, AC online, ACTIVE + sustain    -> "hold"        (BAT_CTRL_STATE_ACTIVE)
 *   adaptive=1, AC online, INACTIVE after hold -> "top-up"      (BAT_CTRL_STATE_INACTIVE + held_this_plug)
 *   adaptive=1, AC online, INACTIVE/SHUTDOWN   -> "inactive"    (BAT_CTRL_STATE_INACTIVE / SHUTDOWN)
 *   adaptive=1, AC unknown                     -> "inactive"
 *
 * Threshold mapping (through existing EC-safe writers and baseline restore):
 *   HOLD:           end = max(hold_percent, current percent), start = end - 5 (if start attr exists)
 *   RESUME/top-up:  99/100 only if holding on this AC plug; otherwise manual/saved baseline
 *   full_until:     99/100 while active on AC; restore manual/saved baseline on unplug/expiry
 *   all other/err:  restore manual limits or saved EC baseline and remove .charge-* marker
 */
static void update_charge_slot(struct slot_state *sl, const struct bat_ac_info *ac,
                               int64_t now, double boot_sec, int64_t full_until,
                               int manual, int force_model_reload, int clock_stepped,
                               int single_pack)
{
    struct bat_device *b = &sl->dev;
    struct bat_charge_slot_status *s = &sl->charge_status;
    memset(s, 0, sizeof *s);
    snprintf(s->slot, sizeof s->slot, "%s", b->name);
    snprintf(s->pack_id, sizeof s->pack_id, "%s", b->pack_id);
    s->requested_end = -1;
    sl->sw_target_end = -1;

    const char *state = "manual";
    int sw_actual_end = -1;
    int managed = charge_cfg.adaptive || full_until > 0;
    int broken_hw_end = !b->has_end_thresh &&
                        present_charge_attr(b, "charge_control_end_threshold") != 0;
    int is_sw = !broken_hw_end && bat_charge_control_kind(b) == BAT_CTL_SW_BEHAVIOUR;
    char path[560];
    struct charge_baseline saved = { 0 };
    int have = charge_baseline_path(b, path, sizeof path) == 0 ? load_charge_baseline(b, path, &saved) : -1;
    if (have < 0) {
        state = "error";
        goto done;
    }

    const char *unsupported = managed ? (single_pack ? charge_check_support(b) : "unsupported") : NULL;
    if (unsupported) {
        if (single_pack && adaptive_ctrl_ready && strcmp(adaptive_ctrl_pack_id, b->pack_id) == 0)
            ctrl_disable_if_enabled(0, now, boot_sec);
        state = unsupported;
        goto manual_limits;
    }

    /* The controller follows the single system pack's AC and charge state for
     * every user, in memory. Its history reaches disk only while adaptive
     * charging is opted in (save_ctrl_history), so opted-out users get no files. */
    struct bat_ctrl_power pwr = ctrl_power_from_device(b, ac);
    int ctrl_bound = 0;
    if (single_pack && ac->system != BAT_AC_UNKNOWN && bat_safe_name(b->pack_id)) {
        ensure_ctrl_for_pack(b, &pwr, now, boot_sec);
        ctrl_bound = 1;
    }

    /* 1. Explicit Charge to Full Now override (works in both manual and adaptive modes). */
    if (full_until > 0 && ac->system == BAT_AC_ONLINE) {
        if (ctrl_bound) {
            (void)bat_ctrl_on_power(&adaptive_ctrl, &pwr, now, boot_sec);
            (void)bat_ctrl_tick(&adaptive_ctrl, now, boot_sec);
            if (adaptive_ctrl.state != BAT_CTRL_STATE_USER_CANCELED)
                (void)bat_ctrl_charge_now(&adaptive_ctrl, now, boot_sec);
            adaptive_held_this_plug = 0;
            adaptive_released_this_plug = 0;
            adaptive_pred_failed = 0;
            alarm_wall_valid = 0;
            (void)save_ctrl_history(0);
        }
        state = "full";
        if (is_sw) {
            s->requested_end = 100;
            if (sw_release_behaviour(b, have, &saved, path, 1) != 0)
                state = "error";
            else
                sw_actual_end = 100;
        } else if (apply_managed_thresholds(b, have, &saved, path, 99, 100, s) != 0) {
            state = "error";
        }
        goto done;
    }

    /* 2. Adaptive charging disabled: manual mode, never touch model or adaptive limits. */
    if (!charge_cfg.adaptive) {
        if (ctrl_bound) {
            ctrl_disable_if_enabled(1, now, boot_sec);
            (void)bat_ctrl_on_power(&adaptive_ctrl, &pwr, now, boot_sec);
            (void)bat_ctrl_tick(&adaptive_ctrl, now, boot_sec);
        }
        unload_adaptive_model();
        state = "manual";
        goto manual_limits;
    }

    /* 3. Adaptive charging opted in: verify model gate first (fail closed). */
    enum model_gate_state mg = model_gate_check(force_model_reload);
    if (mg != MODEL_GATE_READY) {
        if (ctrl_bound) {
            ctrl_disable_if_enabled(1, now, boot_sec);
            (void)bat_ctrl_on_power(&adaptive_ctrl, &pwr, now, boot_sec);
            (void)bat_ctrl_tick(&adaptive_ctrl, now, boot_sec);
            (void)save_ctrl_history(0);
        }
        state = (mg == MODEL_GATE_MISSING) ? "model-missing" : "model-invalid";
        goto manual_limits;
    }

    /* 4. Verified model ready, check AC input state. Keep controller enabled
     * while on battery so the next AC plug-in delivers CHARGER_PLUGGED_IN. */
    if (ac->system == BAT_AC_OFFLINE) {
        if (ctrl_bound) {
            (void)bat_ctrl_on_power(&adaptive_ctrl, &pwr, now, boot_sec);
            if (!adaptive_ctrl.cfg.enabled || !adaptive_ctrl.cfg.supported) {
                struct bat_ctrl_config cfg;
                bat_ctrl_default_config(&cfg);
                cfg.supported = true;
                cfg.enabled = true;
                (void)bat_ctrl_set_config(&adaptive_ctrl, &cfg, now, boot_sec);
            }
            (void)bat_ctrl_tick(&adaptive_ctrl, now, boot_sec);
            (void)save_ctrl_history(0);
        }
        adaptive_held_this_plug = 0;
        adaptive_released_this_plug = 0;
        adaptive_pred_failed = 0;
        alarm_wall_valid = 0;
        state = "on-battery";
        goto manual_limits;
    }
    if (ac->system != BAT_AC_ONLINE || !ctrl_bound) {
        if (adaptive_ctrl_ready)
            ctrl_disable_if_enabled(1, now, boot_sec);
        state = "inactive";
        goto manual_limits;
    }

    /* 5. AC online, supported single pack, verified model ready: drive controller. */
    struct bat_ctrl_decision dec = {
        .action = BAT_CTRL_ACTION_NONE,
        .sustain_percent = -1,
        .release_at = -1.0,
        .target_full_at = -1.0,
        .recheck_at = -1.0,
    };

    if (!adaptive_ctrl.cfg.enabled || !adaptive_ctrl.cfg.supported) {
        /* First ensure controller's live power snapshot is current before enabling
         * so set_config sees ac_connected==true and evaluates immediately. */
        (void)bat_ctrl_on_power(&adaptive_ctrl, &pwr, now, boot_sec);
        struct bat_ctrl_config cfg;
        bat_ctrl_default_config(&cfg);
        cfg.supported = true;
        cfg.enabled = true;
        dec = bat_ctrl_set_config(&adaptive_ctrl, &cfg, now, boot_sec);
        adaptive_held_this_plug = 0;
        adaptive_released_this_plug = 0;
        adaptive_pred_failed = 0;
        alarm_wall_valid = 0;
    } else {
        if (!adaptive_ctrl.power.ac_connected) {
            adaptive_held_this_plug = 0;
            adaptive_released_this_plug = 0;
            adaptive_pred_failed = 0;
            alarm_wall_valid = 0;
        }
        dec = bat_ctrl_on_power(&adaptive_ctrl, &pwr, now, boot_sec);
        if (dec.action == BAT_CTRL_ACTION_RESUME && adaptive_held_this_plug)
            adaptive_released_this_plug = 1;
        struct bat_ctrl_decision tdec = bat_ctrl_tick(&adaptive_ctrl, now, boot_sec);
        if (tdec.action != BAT_CTRL_ACTION_NONE)
            dec = tdec;
        if (dec.action == BAT_CTRL_ACTION_RESUME && adaptive_held_this_plug)
            adaptive_released_this_plug = 1;
    }

    if (adaptive_ctrl.prediction_requested) {
        dec = run_adaptive_prediction(now, boot_sec);
        if (dec.action == BAT_CTRL_ACTION_RESUME && adaptive_held_this_plug && !adaptive_pred_failed)
            adaptive_released_this_plug = 1;
    }
    (void)save_ctrl_history(0);

    /* Map controller state + hold tracking to charge-status and thresholds. */
    switch (adaptive_ctrl.state) {
    case BAT_CTRL_STATE_ACTIVE:
        if (adaptive_ctrl.is_sustain_set) {
            int end;
            if (is_sw) {
                end = adaptive_ctrl.sustain_percent;
                if (end < adaptive_ctrl.cfg.hold_percent) end = adaptive_ctrl.cfg.hold_percent;
                if (end > 100) end = 100;
            } else {
                /* HOLD: end = max(hold_percent, current percent). */
                end = b->capacity_pct > adaptive_ctrl.cfg.hold_percent ?
                      b->capacity_pct : adaptive_ctrl.cfg.hold_percent;
                if (end > 100) end = 100;
            }
            int start = (is_sw || b->has_start_thresh) ? (end >= 5 ? end - 5 : 0) : -1;
            adaptive_held_this_plug = 1;
            adaptive_released_this_plug = 0;
            state = "hold";
            if (adaptive_ctrl.charge_alarm_running && adaptive_ctrl.charge_alarm_at >= 0.0)
                s->release_ts = stable_release_ts(adaptive_ctrl.charge_alarm_at, now, boot_sec, clock_stepped);
            else
                s->release_ts = 0;
            if (is_sw) {
                s->requested_end = end;
                if (sw_enforce_cap(b, ac, have, &saved, path, start, end, &sw_actual_end) != 0)
                    state = "error";
                else if (sw_behaviour_allow_inhibit && end < 100)
                    sl->sw_target_end = end;
            } else if (apply_managed_thresholds(b, have, &saved, path, start, end, s) != 0) {
                state = "error";
            }
            goto done;
        }
        state = "inactive";
        goto manual_limits;

    case BAT_CTRL_STATE_HEURISTIC_DISABLED:
        state = "learning";
        goto manual_limits;

    case BAT_CTRL_STATE_USER_CANCELED:
        /* The Charge to Full Now request has ended (its deadline, or unplug): the
         * override is no longer applied, so this is not "full". No hold until the
         * next plug-in; manual or saved limits apply. */
        state = "inactive";
        goto manual_limits;

    case BAT_CTRL_STATE_INACTIVE:
        if (adaptive_held_this_plug && adaptive_released_this_plug && !adaptive_pred_failed) {
            state = "top-up";
            if (is_sw) {
                s->requested_end = 100;
                if (sw_release_behaviour(b, have, &saved, path, 1) != 0)
                    state = "error";
                else
                    sw_actual_end = 100;
            } else if (apply_managed_thresholds(b, have, &saved, path, 99, 100, s) != 0) {
                state = "error";
            }
            goto done;
        }
        state = "inactive";
        goto manual_limits;

    case BAT_CTRL_STATE_NOT_SUPPORTED:
        state = "unsupported";
        goto manual_limits;

    case BAT_CTRL_STATE_USER_DISABLED:
        state = "manual";
        goto manual_limits;

    case BAT_CTRL_STATE_SHUTDOWN:
        state = "inactive";
        goto manual_limits;
    }

manual_limits:
    if (is_sw) {
        int can_cap = strcmp(state, "error") != 0;
        if (can_cap && ac->system == BAT_AC_ONLINE &&
            charge_cfg.end >= 20 && charge_cfg.end <= 99) {
            int end = charge_cfg.end;
            int start = (charge_cfg.start >= 0 && charge_cfg.start < end)
                        ? charge_cfg.start : (end >= 5 ? end - 5 : 0);
            s->requested_end = end;
            if (sw_enforce_cap(b, ac, have, &saved, path, start, end, &sw_actual_end) != 0)
                state = "error";
            else if (sw_behaviour_allow_inhibit)
                sl->sw_target_end = end;
        } else if (can_cap && charge_cfg.end == 100) {
            s->requested_end = 100;
            if (sw_release_behaviour(b, have, &saved, path, 1) != 0)
                state = "error";
            else
                sw_actual_end = 100;
        } else if (have || b->charge_behaviour == BAT_BEHAV_INHIBIT) {
            if (can_cap && charge_cfg.end >= 20 && charge_cfg.end <= 99)
                s->requested_end = charge_cfg.end;
            if (sw_release_behaviour(b, have, &saved, path, have) != 0)
                state = "error";
        } else if (can_cap && charge_cfg.end >= 20 && charge_cfg.end <= 99) {
            s->requested_end = charge_cfg.end;
        }
    } else if (broken_hw_end && (have || charge_cfg.start >= 0 || charge_cfg.end >= 0)) {
        state = "error";
    } else if (have) {
        int start = charge_cfg.start >= 0 ? charge_cfg.start : saved.start;
        int end = charge_cfg.end >= 0 ? charge_cfg.end : saved.end;
        if (saved.is_behaviour || !b->has_end_thresh || strcmp(saved.end_attr, b->end_attr) ||
            strcmp(saved.start_attr, b->has_start_thresh ? b->start_attr : "-") ||
            !charge_identity_matches(b) || bat_apply_thresholds_to(b, start, end) != 0 ||
            unlink(path) != 0 || sync_history_dir() != 0) {
            state = "error";
            goto done;
        }
        if (b->has_end_thresh) s->requested_end = charge_cfg.end;
    } else if (manual && (charge_cfg.start >= 0 || charge_cfg.end >= 0)) {
        if (b->has_end_thresh) s->requested_end = charge_cfg.end;
        if (!charge_identity_matches(b) || bat_apply_thresholds_to(b, charge_cfg.start, charge_cfg.end) != 0)
            state = "error";
    }
done:
    if (intent_error && charge_cfg.full_until) state = "error";
    if (is_sw)
        s->actual_end = strcmp(state, "error") == 0 ? -1 : sw_actual_end;
    else
        s->actual_end = b->has_end_thresh ? b->end_thresh : -1;
    snprintf(s->state, sizeof s->state, "%s", state);
}

static int scan_for_snapshot(int full_scan, int64_t now_ts, struct bat_device *live, struct bat_ac_info *ac)
{
    int have_occupied = 0;
    for (int j = 0; j < nslots; j++) {
        if (slots[j].occupied) have_occupied++;
    }
    if (full_scan || !daemon_mode || !have_occupied || last_full_scan_ts == 0 ||
        now_ts - last_full_scan_ts >= BAT_STATIC_REFRESH_SEC) {
        int nlive = bat_scan_live(live, BAT_MAX_DEVICES, ac);
        last_full_scan_ts = now_ts;
        return nlive;
    }
    /* Routine timer tick with no power_supply uevent: read only dynamic
     * attributes for tracked slots plus AC online state. Fall back to a full
     * scan immediately if any slot vanished or fails dynamic refresh. */
    if (bat_scan_ac_only(ac) != 0) {
        int nlive = bat_scan_live(live, BAT_MAX_DEVICES, ac);
        last_full_scan_ts = now_ts;
        return nlive;
    }
    int nlive = 0;
    for (int j = 0; j < nslots; j++) {
        if (!slots[j].occupied) continue;
        struct bat_device d = slots[j].dev;
        if (bat_refresh_dynamic(&d, now_ts) != 0) {
            nlive = bat_scan_live(live, BAT_MAX_DEVICES, ac);
            last_full_scan_ts = now_ts;
            return nlive;
        }
        live[nlive++] = d;
    }
    if (!ac->any_adapter && nlive > 0) {
        for (int i = 0; i < nlive; i++) {
            if (!live[i].is_peripheral &&
                (strcmp(live[i].status, "Charging") == 0 || strcmp(live[i].status, "Full") == 0)) {
                ac->online = 1;
            }
        }
    }
    return nlive;
}

static int record_snapshot_ex(int force, int full_scan, int reapply_manual)
{
    struct bat_device live[BAT_MAX_DEVICES];
    struct bat_ac_info ac;
    int64_t now_ts = current_epoch_ts();
    double boot_sec = current_boot_sec(now_ts);
    /* A short sleep or wall-clock adjustment can fit between normal samples.
     * Mark the boundary without scheduling any extra timer. */
    int continuity_break = 0;
    int woke_from_sleep = 0;
    if (daemon_mode) {
        struct timespec monotonic, boot;
        if (clock_gettime(CLOCK_MONOTONIC, &monotonic) == 0 &&
            clock_gettime(CLOCK_BOOTTIME, &boot) == 0) {
            int64_t elapsed = monotonic.tv_sec - last_monotonic;
            int64_t slept = boot.tv_sec - last_boot_time - elapsed;
            int64_t clock_step = now_ts - last_observation - elapsed;
            woke_from_sleep = last_observation && slept > 2;
            continuity_break = !last_observation || slept > 2 ||
                               clock_step > 2 || clock_step < -2;
            last_observation = now_ts;
            last_monotonic = monotonic.tv_sec;
            last_boot_time = boot.tv_sec;
        }
    }
    if (woke_from_sleep && adaptive_ctrl_ready)
        bat_ctrl_on_resume(&adaptive_ctrl, now_ts, boot_sec);

    int nlive = scan_for_snapshot(full_scan, now_ts, live, &ac);
    int manual_slots[BAT_MAX_DEVICES] = { 0 };
    int slot_seen[BAT_MAX_DEVICES] = { 0 };
    int sys_bats = 0;
    int force_all_samples = force;
    int transition = 0;
    int idle_ac = (ac.system == BAT_AC_ONLINE);

    /* 1. Match live system batteries against tracked slots (detect INSERT & SWAP) */
    for (int i = 0; i < nlive; i++) {
        if (live[i].is_peripheral) continue;
        sys_bats++;
        if (strcmp(live[i].status, "Full") != 0 && strcmp(live[i].status, "Not charging") != 0) {
            idle_ac = 0;
        }
        live[i].last_seen_ts = now_ts;
        struct slot_state *sl = get_slot(live[i].name, 1);
        if (!sl) continue;
        int sl_idx = (int)(sl - slots);
        slot_seen[sl_idx] = 1;

        if (!sl->occupied) {
            /* Newly inserted pack (emit INSERT unless this is initial cold discovery) */
            if (sl->ever_seen || any_prior_pack) {
                bat_append_handoff("INSERT", now_ts, sl->slot,
                                   "-", ac.online, live[i].pack_id, live[i].capacity_decipct);
            }
            manual_slots[sl_idx] = 1;
            sl->occupied = 1;
            sl->ever_seen = 1;
            any_prior_pack = 1;
            sl->dev = live[i];
            sl->last_sample_ts = 0;
            sl->npending = 0;
            sl->meta_written = 0;
            force_all_samples = 1;
            transition = 1;
        } else if (strcmp(sl->dev.pack_id, live[i].pack_id) != 0) {
            /* Physical battery pack swapped in the same slot (hot-swap or hibernate cold-swap)! */
            if (adaptive_ctrl_ready && strcmp(adaptive_ctrl_pack_id, sl->dev.pack_id) == 0)
                reset_adaptive_controller(1);
            flush_slot_samples(sl, now_ts);
            int64_t rem_ts = (sl->dev.last_seen_ts > 0 && sl->dev.last_seen_ts < now_ts)
                             ? sl->dev.last_seen_ts + 1 : now_ts;
            struct bat_sample rem = {
                .ts = rem_ts,
                .decipct = sl->dev.capacity_decipct,
                .power_mw = 0,
                .energy_mwh = sl->dev.energy_now_wh >= 0 ? (int)(sl->dev.energy_now_wh * 1000.0 + 0.5) : 0,
                .voltage_mv = 0,
                .state = 'R',
                .ac = ac.online ? 1 : 0,
            };
            bat_append_sample(sl->dev.pack_id, &rem);
            save_slot_meta(sl, 0, sl->dev.last_seen_ts > 0 ? sl->dev.last_seen_ts : now_ts);

            bat_append_handoff("SWAP", now_ts, sl->slot,
                               sl->dev.pack_id, sl->dev.capacity_decipct,
                               live[i].pack_id, live[i].capacity_decipct);
            manual_slots[sl_idx] = 1;

            sl->dev = live[i];
            sl->last_sample_ts = 0;
            sl->npending = 0;
            sl->meta_written = 0;
            force_all_samples = 1;
            transition = 1;
        } else {
            sl->dev = live[i];
        }
    }

    /* 2. Detect slots whose battery pack was removed (hot-unplug on AC or removed while off) */
    for (int j = 0; j < nslots; j++) {
        if (!slots[j].occupied || slot_seen[j]) continue;
        struct slot_state *sl = &slots[j];
        if (adaptive_ctrl_ready && strcmp(adaptive_ctrl_pack_id, sl->dev.pack_id) == 0)
            reset_adaptive_controller(1);
        flush_slot_samples(sl, now_ts);
        int64_t rem_ts = now_ts;
        struct bat_sample rem = {
            .ts = rem_ts,
            .decipct = sl->dev.capacity_decipct,
            .power_mw = 0,
            .energy_mwh = sl->dev.energy_now_wh >= 0 ? (int)(sl->dev.energy_now_wh * 1000.0 + 0.5) : 0,
            .voltage_mv = 0,
            .state = 'R',
            .ac = ac.online ? 1 : 0,
        };
        bat_append_sample(sl->dev.pack_id, &rem);
        save_slot_meta(sl, 0, sl->dev.last_seen_ts > 0 ? sl->dev.last_seen_ts : now_ts);
        bat_append_handoff("REMOVE", now_ts, sl->slot,
                           sl->dev.pack_id, sl->dev.capacity_decipct, "-", ac.online);
        if (strcmp(active_dis_slot, sl->slot) == 0) {
            active_dis_slot[0] = '\0';
            active_dis_pack[0] = '\0';
        }
        sl->occupied = 0;
        sl->last_sample_ts = 0;
        sl->npending = 0;
        force_all_samples = 1;
        transition = 1;
    }

    if (reapply_manual || transition || !daemon_mode) read_charge_config();
    /* On startup/SIGHUP with adaptive opted in, also refresh the model gate even
     * when no system battery slot is currently occupied. */
    if (reapply_manual && charge_cfg.adaptive && sys_bats == 0)
        (void)model_gate_check(1);
    if (sys_bats != 1 && adaptive_ctrl_ready)
        ctrl_disable_if_enabled(0, now_ts, boot_sec);

    struct bat_charge_status charge = {
        .adaptive = charge_cfg.adaptive,
        .full_until = charge_full_until(&ac, now_ts),
    };
    int full_applied = 0;
    int near_cap = 0;
    for (int j = 0; j < nslots; j++) {
        struct slot_state *sl = &slots[j];
        if (!sl->occupied) continue;
        if (continuity_break) {
            struct bat_sample gap;
            charge_sample(&sl->dev, &ac, now_ts, &gap);
            gap.ac_verified = 0;
            queue_slot_sample(sl, &gap, 1);
        }
        update_charge_slot(sl, &ac, now_ts, boot_sec, charge.full_until,
                           reapply_manual || manual_slots[j],
                           reapply_manual, continuity_break, sys_bats == 1);
        if (!strcmp(sl->charge_status.state, "full")) full_applied = 1;
        if (ac.system == BAT_AC_ONLINE &&
            bat_charge_control_kind(&sl->dev) == BAT_CTL_SW_BEHAVIOUR &&
            sl->sw_target_end >= 20 && sl->sw_target_end <= 99 &&
            strcmp(sl->dev.status, "Charging") == 0 &&
            sl->dev.charge_behaviour != BAT_BEHAV_INHIBIT &&
            sl->dev.capacity_pct < sl->sw_target_end &&
            sl->sw_target_end - sl->dev.capacity_pct <= 3) {
            near_cap = 1;
        }
        charge.slots[charge.nslots++] = sl->charge_status;
    }
    if (!full_applied) charge.full_until = 0;
    publish_charge_status(&charge);

    /* 3. Detect dual-battery active discharge handoff (e.g. BAT1 empty -> BAT0 discharging) */
    if (!ac.online) {
        const struct bat_device *cur_dis = NULL;
        for (int i = 0; i < nlive; i++) {
            if (!live[i].is_peripheral && strcmp(live[i].status, "Discharging") == 0) {
                cur_dis = &live[i];
                break;
            }
        }
        if (cur_dis) {
            if (active_dis_slot[0] && strcmp(active_dis_slot, cur_dis->name) != 0) {
                int old_dp = active_dis_dpct;
                for (int i = 0; i < nlive; i++) {
                    if (strcmp(live[i].name, active_dis_slot) == 0) {
                        old_dp = live[i].capacity_decipct;
                        break;
                    }
                }
                char pair[96];
                snprintf(pair, sizeof pair, "%s->%s", active_dis_slot, cur_dis->name);
                bat_append_handoff("DISCHARGE", now_ts, pair,
                                   active_dis_pack, old_dp,
                                   cur_dis->pack_id, cur_dis->capacity_decipct);
                force_all_samples = 1;
                transition = 1;
            }
            snprintf(active_dis_slot, sizeof active_dis_slot, "%s", cur_dis->name);
            snprintf(active_dis_pack, sizeof active_dis_pack, "%s", cur_dis->pack_id);
            active_dis_dpct = cur_dis->capacity_decipct;
        }
    } else {
        active_dis_slot[0] = '\0';
        active_dis_pack[0] = '\0';
    }

    /* 4. Buffer/append curve samples & write pack metadata only on change */
    if (now_ts >= 946684800LL) {
        for (int j = 0; j < nslots; j++) {
            if (!slots[j].occupied) continue;
            struct slot_state *sl = &slots[j];
            struct bat_sample s;
            charge_sample(&sl->dev, &ac, now_ts, &s);
            int changed = sl->last_sample_ts == 0 || sl->last_state != s.state ||
                          sl->last_ac != s.ac || sl->last_ac_verified != s.ac_verified;
            int64_t dt = now_ts - sl->last_sample_ts;
            if (!force_all_samples && !changed && dt >= 0 && dt < 55) continue;
            int immediate = force_all_samples || changed || transition;
            queue_slot_sample(sl, &s, immediate);
            if (pack_meta_changed(sl, &sl->dev)) {
                save_slot_meta(sl, 1, now_ts);
            }
        }
    }
    last_ac_online = ac.online;
    last_system_ac = ac.system;
    all_packs_idle_ac = (sys_bats > 0 && idle_ac);
    sw_near_cap_poll = near_cap;
    return sys_bats;
}

static int record_snapshot(int force)
{
    return record_snapshot_ex(force, 1, force);
}

/* Pre-suspend / SIGUSR1 hook: release any software inhibit-charge FIRST so
 * charging resumes during sleep/hibernate, flush low-power durations, run
 * synchronous SUSPEND prediction if running (without re-latching inhibit), and
 * persist history before sleep. */
static void handle_suspend_flush(void)
{
    int64_t now_ts = current_epoch_ts();
    double boot_sec = current_boot_sec(now_ts);
    release_all_sw_behaviour();
    int prev_allow = sw_behaviour_allow_inhibit;
    sw_behaviour_allow_inhibit = 0;
    if (adaptive_ctrl_ready) {
        (void)bat_ctrl_on_suspend(&adaptive_ctrl, now_ts, boot_sec);
        if (adaptive_ctrl.prediction_requested && charge_cfg.adaptive) {
            if (model_gate_check(0) == MODEL_GATE_READY) {
                struct bat_ctrl_decision d = run_adaptive_prediction(now_ts, boot_sec);
                if (d.action == BAT_CTRL_ACTION_RESUME && adaptive_held_this_plug && !adaptive_pred_failed)
                    adaptive_released_this_plug = 1;
                (void)record_snapshot_ex(0, 0, 0);
            } else {
                ctrl_disable_if_enabled(1, now_ts, boot_sec);
                (void)record_snapshot_ex(0, 0, 0);
            }
        }
        (void)save_ctrl_history(0);
    }
    sw_behaviour_allow_inhibit = prev_allow;
    flush_all_pending(1, now_ts);
}

/* Shutdown / SIGTERM hook: release any software inhibit-charge (both adaptive
 * and manual modes) and unlink its .charge-* baseline, flush low-power
 * durations, persist history, and release any active adaptive HW hold back to
 * manual/saved baseline so the EC is never left holding without a running
 * service. */
static void handle_shutdown(void)
{
    int64_t now_ts = current_epoch_ts();
    double boot_sec = current_boot_sec(now_ts);
    release_all_sw_behaviour();
    if (adaptive_ctrl_ready) {
        struct bat_ctrl_decision d = bat_ctrl_on_shutdown(&adaptive_ctrl, now_ts, boot_sec);
        (void)save_ctrl_history(0);
        if (d.action == BAT_CTRL_ACTION_RESUME) {
            for (int j = 0; j < nslots; j++) {
                struct slot_state *sl = &slots[j];
                if (!sl->occupied || strcmp(sl->dev.pack_id, adaptive_ctrl_pack_id) != 0)
                    continue;
                if (bat_charge_control_kind(&sl->dev) == BAT_CTL_HW_THRESHOLD) {
                    char path[560];
                    struct charge_baseline saved;
                    if (charge_baseline_path(&sl->dev, path, sizeof path) == 0 &&
                        load_charge_baseline(&sl->dev, path, &saved) == 1 &&
                        !saved.is_behaviour &&
                        sl->dev.has_end_thresh && strcmp(saved.end_attr, sl->dev.end_attr) == 0 &&
                        strcmp(saved.start_attr, sl->dev.has_start_thresh ? sl->dev.start_attr : "-") == 0 &&
                        charge_identity_matches(&sl->dev)) {
                        int start = charge_cfg.start >= 0 ? charge_cfg.start : saved.start;
                        int end = charge_cfg.end >= 0 ? charge_cfg.end : saved.end;
                        if (bat_apply_thresholds_to(&sl->dev, start, end) == 0) {
                            (void)unlink(path);
                            (void)sync_history_dir();
                        }
                    }
                }
                break;
            }
        }
    }
    flush_all_pending(1, now_ts);
    adaptive_cleanup();
}

/* Cap poll timeout to the earliest pending controller alarm when adaptive
 * charging is running so release/recheck fire promptly without extra wakeups. */
static int cap_timeout_for_alarms(int timeout_ms)
{
    if (!adaptive_ctrl_ready) return timeout_ms;
    double earliest = -1.0;
    if (adaptive_ctrl.charge_alarm_running && isfinite(adaptive_ctrl.charge_alarm_at))
        earliest = adaptive_ctrl.charge_alarm_at;
    if (adaptive_ctrl.recheck_alarm_running && isfinite(adaptive_ctrl.recheck_alarm_at) &&
        (earliest < 0.0 || adaptive_ctrl.recheck_alarm_at < earliest))
        earliest = adaptive_ctrl.recheck_alarm_at;
    if (earliest < 0.0) return timeout_ms;

    double now_bt = current_boot_sec(current_epoch_ts());
    double rem_ms = (earliest - now_bt) * 1000.0;
    int alarm_ms;
    if (rem_ms <= 0.0) alarm_ms = 100;
    else if (rem_ms >= (double)BAT_POLL_IDLE_AC_MS) return timeout_ms;
    else alarm_ms = (int)rem_ms + 100;

    if (timeout_ms < 0 || alarm_ms < timeout_ms)
        return alarm_ms;
    return timeout_ms;
}

static int compute_poll_timeout_ms(int sys_bats, int have_fds)
{
    int timeout_ms;
    if (sys_bats <= 0) {
        timeout_ms = have_fds ? -1 : BAT_POLL_IDLE_AC_MS;
    } else if (sw_near_cap_poll && last_system_ac == BAT_AC_ONLINE) {
        timeout_ms = BAT_POLL_SW_NEAR_CAP_MS;
    } else if (all_packs_idle_ac && last_ac_online) {
        timeout_ms = BAT_POLL_IDLE_AC_MS;
    } else {
        timeout_ms = BAT_POLL_ACTIVE_MS;
    }
    return cap_timeout_for_alarms(timeout_ms);
}

int main(int argc, char **argv)
{
    int once = argc > 1 && strcmp(argv[1], "--once") == 0;
    if (!getenv("LOCACHY_BATTERY_HISTORY_DIR")) (void)mkdir("/var/lib/locachy", 0755);
    (void)mkdir(bat_history_dir(), 0755);
    char lockpath[560];
    int n = snprintf(lockpath, sizeof lockpath, "%s/.charge.lock", bat_history_dir());
    int lockfd = n > 0 && (size_t)n < sizeof lockpath ?
                 open(lockpath, O_CREAT | O_RDWR | O_CLOEXEC | O_NOFOLLOW, 0600) : -1;
    if (lockfd < 0 || flock(lockfd, LOCK_EX | LOCK_NB) != 0) {
        fprintf(stderr, "battery-history: cannot acquire charge writer lock\n");
        return 1;
    }
    clear_charge_status();
    load_previous_slots();
    if (once) {
        int prev_allow = sw_behaviour_allow_inhibit;
        sw_behaviour_allow_inhibit = 0;
        record_snapshot(1);
        sw_behaviour_allow_inhibit = prev_allow;
        adaptive_cleanup();
        clear_charge_status();
        close(lockfd);
        return 0;
    }

    sw_behaviour_allow_inhibit = 1;
    daemon_mode = 1;

    sigset_t sigmask, oldmask;
    sigemptyset(&sigmask);
    sigaddset(&sigmask, SIGTERM);
    sigaddset(&sigmask, SIGINT);
    sigaddset(&sigmask, SIGUSR1);
    sigaddset(&sigmask, SIGHUP);
    int sfd = -1;
    if (sigprocmask(SIG_BLOCK, &sigmask, &oldmask) == 0) {
        sfd = signalfd(-1, &sigmask, SFD_NONBLOCK | SFD_CLOEXEC);
        if (sfd < 0) sigprocmask(SIG_SETMASK, &oldmask, NULL);
    }
    if (sfd < 0) {
        struct sigaction sa;
        memset(&sa, 0, sizeof sa);
        sa.sa_handler = on_sig;
        sigaction(SIGTERM, &sa, NULL);
        sigaction(SIGINT, &sa, NULL);
        sigaction(SIGUSR1, &sa, NULL);
        sigaction(SIGHUP, &sa, NULL);
    }
    signal(SIGPIPE, SIG_IGN);

    int ufd = open_uevent_socket();
    int sys_bats = record_snapshot_ex(1, 1, 1);

    while (!stop_flag) {
        struct pollfd pfd[2] = {
            { .fd = ufd, .events = POLLIN },
            { .fd = sfd, .events = POLLIN },
        };
        int timeout_ms = compute_poll_timeout_ms(sys_bats, ufd >= 0 && sfd >= 0);
        int r = poll(pfd, 2, timeout_ms);
        if (r > 0 && sfd >= 0) {
            if (pfd[1].revents & (POLLERR | POLLHUP | POLLNVAL)) {
                close(sfd);
                sfd = -1;
                struct sigaction sa;
                memset(&sa, 0, sizeof sa);
                sa.sa_handler = on_sig;
                sigaction(SIGTERM, &sa, NULL);
                sigaction(SIGINT, &sa, NULL);
                sigaction(SIGUSR1, &sa, NULL);
                sigaction(SIGHUP, &sa, NULL);
                sigprocmask(SIG_SETMASK, &oldmask, NULL);
            } else if (pfd[1].revents & POLLIN) {
                struct signalfd_siginfo si;
                for (;;) {
                    ssize_t nr = read(sfd, &si, sizeof si);
                    if (nr != (ssize_t)sizeof si) {
                        if (nr < 0 && errno == EINTR) continue;
                        break;
                    }
                    if (si.ssi_signo == SIGUSR1) flush_flag = 1;
                    else if (si.ssi_signo == SIGHUP) reload_flag = 1;
                    else stop_flag = 1;
                }
            }
        }
        if (stop_flag) break;
        int need_snapshot = 0;
        int full_scan = 0;
        if (r == 0) {
            need_snapshot = 1;
        } else if (r > 0 && ufd >= 0) {
            if (pfd[0].revents & (POLLIN | POLLERR)) {
                int uev = drain_power_uevents(ufd);
                if (uev < 0) {
                    close(ufd);
                    ufd = -1;
                    full_scan = 1;
                    need_snapshot = 1;
                } else if (uev > 0 || (pfd[0].revents & POLLERR)) {
                    if (uev > 0) {
                        /* Give EC 150 ms to settle status/serial/thresholds after pack hot-swap or AC plug. */
                        struct timespec ts = { .tv_sec = 0, .tv_nsec = 150000000L };
                        nanosleep(&ts, NULL);
                    }
                    full_scan = 1;
                    need_snapshot = 1;
                }
            } else if (pfd[0].revents & (POLLHUP | POLLNVAL)) {
                close(ufd);
                ufd = -1;
                full_scan = 1;
                need_snapshot = 1;
            }
        }
        if (reload_flag) {
            reload_flag = 0;
            sys_bats = record_snapshot_ex(0, 1, 1);
            flush_all_pending(1, current_epoch_ts());
        } else if (need_snapshot) {
            sys_bats = record_snapshot_ex(0, full_scan, 0);
        }
        if (flush_flag) {
            flush_flag = 0;
            handle_suspend_flush();
        }
        if (r < 0 && errno != EINTR) {
            struct timespec ts = { .tv_sec = 0, .tv_nsec = 100000000L };
            nanosleep(&ts, NULL);
        }
    }
    handle_shutdown();
    clear_charge_status();
    if (sfd >= 0) {
        close(sfd);
        sigprocmask(SIG_SETMASK, &oldmask, NULL);
    }
    if (lockfd >= 0) close(lockfd);
    return 0;
}
