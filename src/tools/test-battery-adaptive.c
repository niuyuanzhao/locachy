/*
 * Integration tests for battery-history's opt-in adaptive charging:
 *   1. Pin consistency: battery-model-pins.h matches battery-adaptive-manifest.json
 *   2. Model gate (fail closed): missing, wrong size, wrong hash, symlink, invalid
 *      FlatBuffer/protobuf parse, valid fixture, and real ChromiumOS artifacts when
 *      LOCACHY_BATTERY_ARTIFACTS is set
 *   3. Status mapping across every published charge-status state:
 *      manual, on-battery, model-missing, model-invalid, learning, hold (bounded &
 *      unbounded), top-up, inactive, full, unsupported, error
 *   4. Threshold mapping: HOLD max(hold_percent, current percent) & start=end-5
 *      (with and without start threshold), RESUME/top-up (99/100 only when holding
 *      on this plug; manual/baseline otherwise), restore on off/unplug/model error
 *   5. Prediction failure -> controller stop path -> normal/baseline limits
 *   6. ChargeHistory persistence: mode 0600 atomic write, reload, corrupt rejection
 *   7. Opt-out isolation: adaptive=0 leaves manual limits untouched and creates no
 *      controller history file
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <errno.h>
#include <limits.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define main battery_history_main
#include "../progs/battery-history.c"
#undef main

static int g_pass;
static int g_fail;
static char g_root[1024];

#define CHECK(cond) do { \
    if (!(cond)) { \
        fprintf(stderr, "  check failed at %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        return false; \
    } \
} while (0)

static void run_test(const char *desc, bool (*fn)(void))
{
    if (fn()) {
        printf("PASS %s\n", desc);
        g_pass++;
    } else {
        printf("FAIL %s\n", desc);
        g_fail++;
    }
}

/* ---- Filesystem and fixture helpers ---- */

static void mkd_abs(const char *full)
{
    char cur[1400];
    size_t len = strlen(full);
    if (len >= sizeof cur) return;
    for (size_t i = 1; i <= len; i++) {
        if (full[i] == '/' || full[i] == '\0') {
            memcpy(cur, full, i);
            cur[i] = '\0';
            (void)mkdir(cur, 0755);
        }
    }
}

static void rm_rf_dir(const char *dir)
{
    DIR *d = opendir(dir);
    if (!d) {
        (void)unlink(dir);
        return;
    }
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        char sub[1600];
        snprintf(sub, sizeof sub, "%s/%s", dir, e->d_name);
        struct stat st;
        if (lstat(sub, &st) == 0 && S_ISDIR(st.st_mode)) rm_rf_dir(sub);
        else (void)unlink(sub);
    }
    closedir(d);
    (void)rmdir(dir);
}

static char g_case[1280];

static void setup_case(const char *sub)
{
    static int seq = 0;
    snprintf(g_case, sizeof g_case, "%s/case-%d-%s", g_root, ++seq, sub);
    rm_rf_dir(g_case);
    char p_sys[1400], p_hist[1400], p_rc[1400], p_bl[1400];
    snprintf(p_sys, sizeof p_sys, "%s/sys", g_case);
    snprintf(p_hist, sizeof p_hist, "%s/hist", g_case);
    snprintf(p_rc, sizeof p_rc, "%s/rc.conf", g_case);
    snprintf(p_bl, sizeof p_bl, "%s/backlight", g_case);
    mkd_abs(p_sys);
    mkd_abs(p_hist);
    mkd_abs(p_bl);

    adaptive_cleanup();
    active_model_pins = &prod_model_pins;
    bat_predict_override = NULL;
    memset(slots, 0, sizeof slots);
    nslots = 0;
    any_prior_pack = 0;
    active_dis_slot[0] = '\0';
    active_dis_pack[0] = '\0';
    active_dis_dpct = 0;
    daemon_mode = 0;
    last_full_scan_ts = 0;
    last_ac_online = 0;
    last_system_ac = BAT_AC_UNKNOWN;
    all_packs_idle_ac = 0;
    cancelled_full_until = 0;
    intent_loaded = intent_error = 0;
    charge_cfg = (struct charge_config){ .start = -1, .end = -1 };

    setenv("LOCACHY_POWER_SUPPLY_DIR", p_sys, 1);
    setenv("LOCACHY_BATTERY_HISTORY_DIR", p_hist, 1);
    setenv("LOCACHY_RC_CONF", p_rc, 1);
    setenv("LOCACHY_BACKLIGHT_DIR", p_bl, 1);
    unsetenv("LOCACHY_BOOT_SEC");
}

static void case_mkd(const char *rel)
{
    char full[1500];
    snprintf(full, sizeof full, "%s/%s", g_case, rel);
    mkd_abs(full);
}

static void case_put_txt(const char *rel, const char *txt)
{
    char p[1500];
    snprintf(p, sizeof p, "%s/%s", g_case, rel);
    FILE *f = fopen(p, "w");
    if (!f) abort();
    if (fputs(txt, f) < 0 || fclose(f) != 0) abort();
}

static void case_put_bin(const char *rel, const void *buf, size_t len)
{
    char p[1500];
    snprintf(p, sizeof p, "%s/%s", g_case, rel);
    FILE *f = fopen(p, "wb");
    if (!f) abort();
    if (len > 0 && fwrite(buf, 1, len, f) != len) abort();
    if (fclose(f) != 0) abort();
}

static int case_get_int(const char *rel)
{
    char p[1500], b[64] = "";
    snprintf(p, sizeof p, "%s/%s", g_case, rel);
    FILE *f = fopen(p, "r");
    if (!f || !fgets(b, sizeof b, f)) {
        if (f) fclose(f);
        return -999;
    }
    fclose(f);
    return atoi(b);
}

static void case_unlink(const char *rel)
{
    char p[1500];
    snprintf(p, sizeof p, "%s/%s", g_case, rel);
    (void)unlink(p);
}

static int case_exists(const char *rel)
{
    char p[1500];
    snprintf(p, sizeof p, "%s/%s", g_case, rel);
    struct stat st;
    return lstat(p, &st) == 0;
}

static void set_clock(int64_t wall_ts, double boot_sec)
{
    char wb[32], bb[48];
    snprintf(wb, sizeof wb, "%lld", (long long)wall_ts);
    setenv("LOCACHY_NOW_TS", wb, 1);
    if (boot_sec > 0.0) {
        snprintf(bb, sizeof bb, "%.3f", boot_sec);
        setenv("LOCACHY_BOOT_SEC", bb, 1);
    } else {
        unsetenv("LOCACHY_BOOT_SEC");
    }
}

static void create_single_pack(int ac_online, int pct, const char *status_str,
                               int start_thresh, int end_thresh)
{
    case_mkd("sys/AC");
    case_put_txt("sys/AC/type", "Mains\n");
    case_put_txt("sys/AC/online", ac_online ? "1\n" : "0\n");

    case_mkd("sys/BAT0");
    case_put_txt("sys/BAT0/type", "Battery\n");
    case_put_txt("sys/BAT0/present", "1\n");
    char buf[64];
    snprintf(buf, sizeof buf, "%s\n", status_str);
    case_put_txt("sys/BAT0/status", buf);
    case_put_txt("sys/BAT0/manufacturer", "Lenovo\n");
    case_put_txt("sys/BAT0/model_name", "5B10W139\n");
    case_put_txt("sys/BAT0/serial_number", "4242\n");
    snprintf(buf, sizeof buf, "%d\n", pct * 500000);
    case_put_txt("sys/BAT0/energy_now", buf);
    case_put_txt("sys/BAT0/energy_full", "50000000\n");
    case_put_txt("sys/BAT0/energy_full_design", "50000000\n");
    if (start_thresh >= 0) {
        snprintf(buf, sizeof buf, "%d\n", start_thresh);
        case_put_txt("sys/BAT0/charge_control_start_threshold", buf);
    } else {
        case_unlink("sys/BAT0/charge_control_start_threshold");
    }
    if (end_thresh > 0) {
        snprintf(buf, sizeof buf, "%d\n", end_thresh);
        case_put_txt("sys/BAT0/charge_control_end_threshold", buf);
    } else {
        case_unlink("sys/BAT0/charge_control_end_threshold");
    }
}

static void sha512_hex(const uint8_t *buf, size_t len, char hex[129])
{
    unsigned char md[EVP_MAX_MD_SIZE];
    unsigned int md_len = 0;
    EVP_MD_CTX *ctx = EVP_MD_CTX_new();
    if (!ctx || EVP_DigestInit_ex(ctx, EVP_sha512(), NULL) != 1 ||
        EVP_DigestUpdate(ctx, buf, len) != 1 ||
        EVP_DigestFinal_ex(ctx, md, &md_len) != 1 || md_len != 64)
        abort();
    EVP_MD_CTX_free(ctx);
    for (int i = 0; i < 64; i++)
        snprintf(hex + i * 2, 3, "%02x", md[i]);
    hex[128] = '\0';
}

/* ---- Minimal valid TFLite + ExamplePreprocessor fixture builder ---- */

struct fbw {
    uint8_t *buf;
    size_t len, cap;
};

static void fbw_grow(struct fbw *w, size_t n)
{
    if (w->len + n <= w->cap) return;
    size_t c = w->cap ? w->cap * 2 : 512;
    while (c < w->len + n) c *= 2;
    w->buf = realloc(w->buf, c);
    if (!w->buf) abort();
    w->cap = c;
}

static void fbw_put(struct fbw *w, const void *p, size_t n)
{
    fbw_grow(w, n);
    memcpy(w->buf + w->len, p, n);
    w->len += n;
}

static void fbw_u8(struct fbw *w, uint8_t v) { fbw_put(w, &v, 1); }

static void fbw_u16(struct fbw *w, uint16_t v)
{
    uint8_t b[2] = { (uint8_t)v, (uint8_t)(v >> 8) };
    fbw_put(w, b, 2);
}

static void fbw_u32(struct fbw *w, uint32_t v)
{
    uint8_t b[4] = { (uint8_t)v, (uint8_t)(v >> 8), (uint8_t)(v >> 16), (uint8_t)(v >> 24) };
    fbw_put(w, b, 4);
}

static void fbw_i32(struct fbw *w, int32_t v)
{
    uint32_t u;
    memcpy(&u, &v, 4);
    fbw_u32(w, u);
}

static void fbw_f32(struct fbw *w, float v)
{
    uint32_t u;
    memcpy(&u, &v, 4);
    fbw_u32(w, u);
}

static void fbw_align4(struct fbw *w) { while (w->len & 3u) fbw_u8(w, 0); }

static void fbw_patch_u32(struct fbw *w, size_t pos, uint32_t v)
{
    w->buf[pos + 0] = (uint8_t)v;
    w->buf[pos + 1] = (uint8_t)(v >> 8);
    w->buf[pos + 2] = (uint8_t)(v >> 16);
    w->buf[pos + 3] = (uint8_t)(v >> 24);
}

static uint8_t *build_fixture_tflite(int hot_bucket, size_t *out_len)
{
    struct fbw w = {0};
    fbw_u32(&w, 0);
    fbw_put(&w, "TFL3", 4);

    size_t m_vt = w.len;
    fbw_u16(&w, 14); fbw_u16(&w, 20);
    fbw_u16(&w, 4); fbw_u16(&w, 8); fbw_u16(&w, 12); fbw_u16(&w, 0); fbw_u16(&w, 16);
    fbw_align4(&w);
    size_t m_pos = w.len;
    fbw_patch_u32(&w, 0, (uint32_t)m_pos);
    fbw_i32(&w, (int32_t)(m_pos - m_vt));
    fbw_u32(&w, 3);
    size_t m_oc = w.len; fbw_u32(&w, 0);
    size_t m_sg = w.len; fbw_u32(&w, 0);
    size_t m_bf = w.len; fbw_u32(&w, 0);

    size_t ocv = w.len;
    fbw_patch_u32(&w, m_oc, (uint32_t)(ocv - m_oc));
    fbw_u32(&w, 2);
    size_t ocv0 = w.len; fbw_u32(&w, 0);
    size_t ocv1 = w.len; fbw_u32(&w, 0);
    size_t ocvt = w.len;
    fbw_u16(&w, 6); fbw_u16(&w, 8); fbw_u16(&w, 4);
    fbw_align4(&w);
    size_t oct0 = w.len;
    fbw_patch_u32(&w, ocv0, (uint32_t)(oct0 - ocv0));
    fbw_i32(&w, (int32_t)(oct0 - ocvt));
    fbw_u8(&w, 9); fbw_u8(&w, 0); fbw_u8(&w, 0); fbw_u8(&w, 0);
    size_t oct1 = w.len;
    fbw_patch_u32(&w, ocv1, (uint32_t)(oct1 - ocv1));
    fbw_i32(&w, (int32_t)(oct1 - ocvt));
    fbw_u8(&w, 25); fbw_u8(&w, 0); fbw_u8(&w, 0); fbw_u8(&w, 0);

    size_t bfv = w.len;
    fbw_patch_u32(&w, m_bf, (uint32_t)(bfv - m_bf));
    fbw_u32(&w, 3);
    size_t bfv_s[3];
    for (int i = 0; i < 3; i++) { bfv_s[i] = w.len; fbw_u32(&w, 0); }
    size_t bfvt_empty = w.len;
    fbw_u16(&w, 4); fbw_u16(&w, 4);
    fbw_align4(&w);
    size_t bfvt_data = w.len;
    fbw_u16(&w, 6); fbw_u16(&w, 8); fbw_u16(&w, 4);
    fbw_align4(&w);

    size_t bt0 = w.len;
    fbw_patch_u32(&w, bfv_s[0], (uint32_t)(bt0 - bfv_s[0]));
    fbw_i32(&w, (int32_t)(bt0 - bfvt_empty));

    size_t bt1 = w.len;
    fbw_patch_u32(&w, bfv_s[1], (uint32_t)(bt1 - bfv_s[1]));
    fbw_i32(&w, (int32_t)(bt1 - bfvt_data));
    size_t bt1_d = w.len; fbw_u32(&w, 0);
    size_t wdata = w.len;
    fbw_patch_u32(&w, bt1_d, (uint32_t)(wdata - bt1_d));
    fbw_u32(&w, 9 * 4);
    for (int i = 0; i < 9; i++) fbw_f32(&w, 0.0f);

    size_t bt2 = w.len;
    fbw_patch_u32(&w, bfv_s[2], (uint32_t)(bt2 - bfv_s[2]));
    fbw_i32(&w, (int32_t)(bt2 - bfvt_data));
    size_t bt2_d = w.len; fbw_u32(&w, 0);
    size_t bdata = w.len;
    fbw_patch_u32(&w, bt2_d, (uint32_t)(bdata - bt2_d));
    fbw_u32(&w, 9 * 4);
    for (int i = 0; i < 9; i++) fbw_f32(&w, i == hot_bucket ? 10.0f : -10.0f);

    size_t sgv = w.len;
    fbw_patch_u32(&w, m_sg, (uint32_t)(sgv - m_sg));
    fbw_u32(&w, 1);
    size_t sgv0 = w.len; fbw_u32(&w, 0);
    size_t sgvt = w.len;
    fbw_u16(&w, 12); fbw_u16(&w, 20);
    fbw_u16(&w, 4); fbw_u16(&w, 8); fbw_u16(&w, 12); fbw_u16(&w, 16);
    fbw_align4(&w);
    size_t sgt = w.len;
    fbw_patch_u32(&w, sgv0, (uint32_t)(sgt - sgv0));
    fbw_i32(&w, (int32_t)(sgt - sgvt));
    size_t sg_t = w.len; fbw_u32(&w, 0);
    size_t sg_in = w.len; fbw_u32(&w, 0);
    size_t sg_out = w.len; fbw_u32(&w, 0);
    size_t sg_ops = w.len; fbw_u32(&w, 0);

    size_t inv = w.len;
    fbw_patch_u32(&w, sg_in, (uint32_t)(inv - sg_in));
    fbw_u32(&w, 1); fbw_i32(&w, 0);
    size_t outv = w.len;
    fbw_patch_u32(&w, sg_out, (uint32_t)(outv - sg_out));
    fbw_u32(&w, 1); fbw_i32(&w, 4);

    size_t tv = w.len;
    fbw_patch_u32(&w, sg_t, (uint32_t)(tv - sg_t));
    fbw_u32(&w, 5);
    size_t tv_s[5];
    for (int i = 0; i < 5; i++) { tv_s[i] = w.len; fbw_u32(&w, 0); }
    size_t tvt = w.len;
    fbw_u16(&w, 10); fbw_u16(&w, 12);
    fbw_u16(&w, 4); fbw_u16(&w, 0); fbw_u16(&w, 8);
    fbw_align4(&w);

    const int32_t shapes[5][3] = {
        { 2, 1, 1 }, { 2, 9, 1 }, { 1, 9, 0 }, { 2, 1, 9 }, { 2, 1, 9 }
    };
    const uint32_t bufs[5] = { 0, 1, 2, 0, 0 };
    for (int i = 0; i < 5; i++) {
        size_t tp = w.len;
        fbw_patch_u32(&w, tv_s[i], (uint32_t)(tp - tv_s[i]));
        fbw_i32(&w, (int32_t)(tp - tvt));
        size_t sh_s = w.len; fbw_u32(&w, 0);
        fbw_u32(&w, bufs[i]);
        size_t sh_v = w.len;
        fbw_patch_u32(&w, sh_s, (uint32_t)(sh_v - sh_s));
        fbw_u32(&w, (uint32_t)shapes[i][0]);
        fbw_i32(&w, shapes[i][1]);
        if (shapes[i][0] == 2) fbw_i32(&w, shapes[i][2]);
    }

    size_t opv = w.len;
    fbw_patch_u32(&w, sg_ops, (uint32_t)(opv - sg_ops));
    fbw_u32(&w, 2);
    size_t opv0 = w.len; fbw_u32(&w, 0);
    size_t opv1 = w.len; fbw_u32(&w, 0);

    size_t opvt_fc = w.len;
    fbw_u16(&w, 10); fbw_u16(&w, 16);
    fbw_u16(&w, 4); fbw_u16(&w, 8); fbw_u16(&w, 12);
    fbw_align4(&w);
    size_t op0 = w.len;
    fbw_patch_u32(&w, opv0, (uint32_t)(op0 - opv0));
    fbw_i32(&w, (int32_t)(op0 - opvt_fc));
    fbw_u32(&w, 0);
    size_t op0_in = w.len; fbw_u32(&w, 0);
    size_t op0_out = w.len; fbw_u32(&w, 0);
    size_t op0_inv = w.len;
    fbw_patch_u32(&w, op0_in, (uint32_t)(op0_inv - op0_in));
    fbw_u32(&w, 3); fbw_i32(&w, 0); fbw_i32(&w, 1); fbw_i32(&w, 2);
    size_t op0_outv = w.len;
    fbw_patch_u32(&w, op0_out, (uint32_t)(op0_outv - op0_out));
    fbw_u32(&w, 1); fbw_i32(&w, 3);

    size_t opvt_sm = w.len;
    fbw_u16(&w, 14); fbw_u16(&w, 24);
    fbw_u16(&w, 4); fbw_u16(&w, 8); fbw_u16(&w, 12); fbw_u16(&w, 16); fbw_u16(&w, 20);
    fbw_align4(&w);
    size_t smopt_vt = w.len;
    fbw_u16(&w, 6); fbw_u16(&w, 8); fbw_u16(&w, 4);
    fbw_align4(&w);
    size_t op1 = w.len;
    fbw_patch_u32(&w, opv1, (uint32_t)(op1 - opv1));
    fbw_i32(&w, (int32_t)(op1 - opvt_sm));
    fbw_u32(&w, 1);
    size_t op1_in = w.len; fbw_u32(&w, 0);
    size_t op1_out = w.len; fbw_u32(&w, 0);
    fbw_u8(&w, 9); fbw_u8(&w, 0); fbw_u8(&w, 0); fbw_u8(&w, 0);
    size_t op1_opt = w.len; fbw_u32(&w, 0);
    size_t op1_inv = w.len;
    fbw_patch_u32(&w, op1_in, (uint32_t)(op1_inv - op1_in));
    fbw_u32(&w, 1); fbw_i32(&w, 3);
    size_t op1_outv = w.len;
    fbw_patch_u32(&w, op1_out, (uint32_t)(op1_outv - op1_out));
    fbw_u32(&w, 1); fbw_i32(&w, 4);
    size_t smopt = w.len;
    fbw_patch_u32(&w, op1_opt, (uint32_t)(smopt - op1_opt));
    fbw_i32(&w, (int32_t)(smopt - smopt_vt));
    fbw_f32(&w, 1.0f);

    *out_len = w.len;
    return w.buf;
}

static uint8_t *build_fixture_preproc(size_t *out_len)
{
    static const uint8_t pb[] = {
        0x1a, 0x0e,
        0x0a, 0x0a, 'I', 's', 'C', 'h', 'a', 'r', 'g', 'i', 'n', 'g',
        0x10, 0x00
    };
    uint8_t *copy = malloc(sizeof pb);
    if (!copy) abort();
    memcpy(copy, pb, sizeof pb);
    *out_len = sizeof pb;
    return copy;
}

struct fixture_model {
    uint8_t *tfl;
    size_t tfl_len;
    uint8_t *pb;
    size_t pb_len;
    char tfl_sha[129];
    char pb_sha[129];
    struct bat_model_pin_spec pins;
};

static void install_fixture_model(struct fixture_model *fm, int hot_bucket)
{
    memset(fm, 0, sizeof *fm);
    fm->tfl = build_fixture_tflite(hot_bucket, &fm->tfl_len);
    fm->pb = build_fixture_preproc(&fm->pb_len);
    sha512_hex(fm->tfl, fm->tfl_len, fm->tfl_sha);
    sha512_hex(fm->pb, fm->pb_len, fm->pb_sha);
    fm->pins = (struct bat_model_pin_spec){
        .tflite_name    = BAT_MODEL_TFLITE_NAME,
        .tflite_size    = fm->tfl_len,
        .tflite_sha512  = fm->tfl_sha,
        .preproc_name   = BAT_MODEL_PREPROC_NAME,
        .preproc_size   = fm->pb_len,
        .preproc_sha512 = fm->pb_sha,
    };
    active_model_pins = &fm->pins;
    case_mkd("hist/model");
    case_put_bin("hist/model/" BAT_MODEL_TFLITE_NAME, fm->tfl, fm->tfl_len);
    case_put_bin("hist/model/" BAT_MODEL_PREPROC_NAME, fm->pb, fm->pb_len);
}

static void free_fixture_model(struct fixture_model *fm)
{
    active_model_pins = &prod_model_pins;
    adaptive_cleanup();
    free(fm->tfl);
    free(fm->pb);
    memset(fm, 0, sizeof *fm);
}

static void write_history_for_pack(const char *pack_id, int ndays, int on_ac_h,
                                   int full_h, int event_dur_h, int64_t now_ts, double boot_sec)
{
    struct bat_ctrl tmp;
    if (bat_ctrl_init(&tmp, NULL) != 0) abort();
    struct bat_history_snapshot snap;
    memset(&snap, 0, sizeof snap);
    int64_t today = bat_hist_utc_midnight(now_ts);
    for (int i = 0; i < ndays; i++) {
        snap.days[snap.day_count++] = (struct bat_daily_summary){
            .day_utc_sec = today - (int64_t)(ndays - 1 - i) * BAT_CTRL_DAY_SEC,
            .time_on_ac_sec = (int64_t)on_ac_h * 3600,
            .time_full_on_ac_sec = (int64_t)full_h * 3600,
            .hold_time_on_ac_sec = 0,
        };
    }
    for (int i = 0; i < 10; i++) {
        snap.events[snap.event_count++] = (struct bat_charge_event){
            .start_wall_sec = today - (int64_t)(10 - i) * 12 * 3600,
            .duration_sec = (int64_t)event_dur_h * 3600,
        };
    }
    struct bat_ctrl_power off = { false, false, 60.0, 60.0 };
    if (bat_ctrl_import_history(&tmp, &snap, &off, now_ts, boot_sec) != 0) abort();

    int64_t buf[BAT_CTRL_SERIAL_MAX_I64];
    int n = bat_ctrl_serialize_history(&tmp, buf, BAT_CTRL_SERIAL_MAX_I64);
    if (n <= 0) abort();
    char rel[256];
    snprintf(rel, sizeof rel, "hist/charge-history-%s.bin", pack_id);
    case_put_bin(rel, buf, (size_t)n * sizeof(int64_t));
}

static bool read_slot_status(const char *slot, struct bat_charge_slot_status *out,
                             struct bat_charge_status *full_out)
{
    struct bat_charge_status st;
    if (bat_read_charge_status(&st) != 0) return false;
    if (full_out) *full_out = st;
    for (int i = 0; i < st.nslots; i++) {
        if (strcmp(st.slots[i].slot, slot) == 0) {
            if (out) *out = st.slots[i];
            return true;
        }
    }
    return false;
}

/* ---- Test 1: Pin consistency against battery-adaptive-manifest.json ---- */

static bool json_extract_section_str(const char *json, const char *section,
                                     const char *key, char *out, size_t cap)
{
    char sec_pat[64], key_pat[64];
    snprintf(sec_pat, sizeof sec_pat, "\"%s\"", section);
    snprintf(key_pat, sizeof key_pat, "\"%s\"", key);
    const char *sp = strstr(json, sec_pat);
    if (!sp) return false;
    const char *brace = strchr(sp, '{');
    if (!brace) return false;
    const char *end_brace = strchr(brace, '}');
    if (!end_brace) return false;
    const char *kp = strstr(brace, key_pat);
    if (!kp || kp >= end_brace) return false;
    const char *colon = strchr(kp + strlen(key_pat), ':');
    if (!colon || colon >= end_brace) return false;
    const char *q1 = strchr(colon, '"');
    if (!q1 || q1 >= end_brace) return false;
    q1++;
    const char *q2 = strchr(q1, '"');
    if (!q2 || q2 >= end_brace) return false;
    size_t len = (size_t)(q2 - q1);
    if (len >= cap) return false;
    memcpy(out, q1, len);
    out[len] = '\0';
    return true;
}

static bool json_extract_section_u64(const char *json, const char *section,
                                     const char *key, uint64_t *out)
{
    char sec_pat[64], key_pat[64];
    snprintf(sec_pat, sizeof sec_pat, "\"%s\"", section);
    snprintf(key_pat, sizeof key_pat, "\"%s\"", key);
    const char *sp = strstr(json, sec_pat);
    if (!sp) return false;
    const char *brace = strchr(sp, '{');
    if (!brace) return false;
    const char *end_brace = strchr(brace, '}');
    if (!end_brace) return false;
    const char *kp = strstr(brace, key_pat);
    if (!kp || kp >= end_brace) return false;
    const char *colon = strchr(kp + strlen(key_pat), ':');
    if (!colon || colon >= end_brace) return false;
    colon++;
    while (*colon == ' ' || *colon == '\t') colon++;
    char *ep = NULL;
    errno = 0;
    unsigned long long v = strtoull(colon, &ep, 10);
    if (errno || ep == colon || ep > end_brace) return false;
    *out = (uint64_t)v;
    return true;
}

static bool test_manifest_pin_consistency(void)
{
    const char *candidates[4] = {
        getenv("LOCACHY_BATTERY_MANIFEST"),
        NULL,
        "src/link/battery-adaptive-manifest.json",
        "../link/battery-adaptive-manifest.json",
    };
    /* Resolve relative to __FILE__ (src/tools/test-battery-adaptive.c). */
    char from_file[PATH_MAX] = "";
    const char *slash = strrchr(__FILE__, '/');
    if (slash) {
        size_t dlen = (size_t)(slash - __FILE__);
        if (dlen + 48 < sizeof from_file) {
            memcpy(from_file, __FILE__, dlen);
            snprintf(from_file + dlen, sizeof from_file - dlen,
                     "/../link/battery-adaptive-manifest.json");
            candidates[1] = from_file;
        }
    }

    FILE *f = NULL;
    for (size_t i = 0; i < 4 && !f; i++) {
        if (candidates[i] && candidates[i][0]) f = fopen(candidates[i], "r");
    }
    CHECK(f != NULL);

    char json[8192];
    size_t nr = fread(json, 1, sizeof json - 1, f);
    fclose(f);
    CHECK(nr > 0 && nr < sizeof json - 1);
    json[nr] = '\0';

    char m_name[128], m_sha[160], p_name[128], p_sha[160];
    uint64_t m_size = 0, p_size = 0;
    CHECK(json_extract_section_str(json, "model", "filename", m_name, sizeof m_name));
    CHECK(json_extract_section_u64(json, "model", "size", &m_size));
    CHECK(json_extract_section_str(json, "model", "sha512", m_sha, sizeof m_sha));
    CHECK(json_extract_section_str(json, "preprocessor", "filename", p_name, sizeof p_name));
    CHECK(json_extract_section_u64(json, "preprocessor", "size", &p_size));
    CHECK(json_extract_section_str(json, "preprocessor", "sha512", p_sha, sizeof p_sha));

    CHECK(strcmp(m_name, BAT_MODEL_TFLITE_NAME) == 0);
    CHECK(m_size == (uint64_t)BAT_MODEL_TFLITE_SIZE);
    CHECK(strcmp(m_sha, BAT_MODEL_TFLITE_SHA512) == 0);
    CHECK(strcmp(p_name, BAT_MODEL_PREPROC_NAME) == 0);
    CHECK(p_size == (uint64_t)BAT_MODEL_PREPROC_SIZE);
    CHECK(strcmp(p_sha, BAT_MODEL_PREPROC_SHA512) == 0);
    return true;
}

/* ---- Test 2: Model gate (missing, wrong size, wrong hash, symlink, parse fail, valid) ---- */

static bool test_model_gate_fixtures(void)
{
    const int64_t t0 = 1769817600LL;
    setup_case("model-gate");
    create_single_pack(1, 60, "Charging", 55, 85);
    case_put_txt("rc.conf", "battery_charge_adaptive=\"1\"\n");
    set_clock(t0, 100.0);

    /* 2a. Missing model directory -> model-missing, thresholds untouched. */
    CHECK(record_snapshot(1) == 1);
    struct bat_charge_slot_status ss;
    CHECK(read_slot_status("BAT0", &ss, NULL));
    CHECK(strcmp(ss.state, "model-missing") == 0);
    CHECK(case_get_int("sys/BAT0/charge_control_start_threshold") == 55);
    CHECK(case_get_int("sys/BAT0/charge_control_end_threshold") == 85);

    /* 2b. Install valid fixture -> ready (controller sees <14d -> learning). */
    struct fixture_model fm;
    install_fixture_model(&fm, 8);
    CHECK(model_gate_check(1) == MODEL_GATE_READY);
    CHECK(adaptive_model != NULL);

    /* 2c. Remove preprocessor only -> model-missing, cached model unloaded. */
    case_unlink("hist/model/" BAT_MODEL_PREPROC_NAME);
    CHECK(record_snapshot(1) == 1);
    CHECK(read_slot_status("BAT0", &ss, NULL));
    CHECK(strcmp(ss.state, "model-missing") == 0);
    CHECK(adaptive_model == NULL);

    /* 2d. Wrong size -> model-invalid. */
    case_put_bin("hist/model/" BAT_MODEL_PREPROC_NAME, fm.pb, fm.pb_len - 1);
    CHECK(record_snapshot(1) == 1);
    CHECK(read_slot_status("BAT0", &ss, NULL));
    CHECK(strcmp(ss.state, "model-invalid") == 0);
    CHECK(adaptive_model == NULL);

    /* 2e. Right size, flipped byte (wrong SHA-512) -> model-invalid. */
    uint8_t *bad_pb = malloc(fm.pb_len);
    CHECK(bad_pb != NULL);
    memcpy(bad_pb, fm.pb, fm.pb_len);
    bad_pb[fm.pb_len - 1] ^= 0x01;
    case_put_bin("hist/model/" BAT_MODEL_PREPROC_NAME, bad_pb, fm.pb_len);
    free(bad_pb);
    CHECK(record_snapshot(1) == 1);
    CHECK(read_slot_status("BAT0", &ss, NULL));
    CHECK(strcmp(ss.state, "model-invalid") == 0);

    /* 2f. Symlink to valid artifact refused via O_NOFOLLOW -> model-invalid. */
    case_put_bin("hist/real-preproc.pb", fm.pb, fm.pb_len);
    case_unlink("hist/model/" BAT_MODEL_PREPROC_NAME);
    char target[1500], linkp[1500];
    snprintf(target, sizeof target, "%s/hist/real-preproc.pb", g_case);
    snprintf(linkp, sizeof linkp, "%s/hist/model/%s", g_case, BAT_MODEL_PREPROC_NAME);
    CHECK(symlink(target, linkp) == 0);
    CHECK(record_snapshot(1) == 1);
    CHECK(read_slot_status("BAT0", &ss, NULL));
    CHECK(strcmp(ss.state, "model-invalid") == 0);
    case_unlink("hist/model/" BAT_MODEL_PREPROC_NAME);

    /* 2g. Valid SHA-512 & size, but corrupt TFLite bytes (parse failure) -> model-invalid. */
    uint8_t bogus_tfl[64];
    memset(bogus_tfl, 0x5a, sizeof bogus_tfl);
    char bogus_sha[129];
    sha512_hex(bogus_tfl, sizeof bogus_tfl, bogus_sha);
    struct bat_model_pin_spec bogus_pins = fm.pins;
    bogus_pins.tflite_size = sizeof bogus_tfl;
    bogus_pins.tflite_sha512 = bogus_sha;
    active_model_pins = &bogus_pins;
    case_put_bin("hist/model/" BAT_MODEL_TFLITE_NAME, bogus_tfl, sizeof bogus_tfl);
    case_put_bin("hist/model/" BAT_MODEL_PREPROC_NAME, fm.pb, fm.pb_len);
    CHECK(record_snapshot(1) == 1);
    CHECK(read_slot_status("BAT0", &ss, NULL));
    CHECK(strcmp(ss.state, "model-invalid") == 0);
    CHECK(adaptive_model == NULL);

    /* 2h. Restore valid fixture -> ready and thresholds remain baseline (learning). */
    active_model_pins = &fm.pins;
    case_put_bin("hist/model/" BAT_MODEL_TFLITE_NAME, fm.tfl, fm.tfl_len);
    CHECK(record_snapshot(1) == 1);
    CHECK(read_slot_status("BAT0", &ss, NULL));
    CHECK(strcmp(ss.state, "learning") == 0);
    CHECK(case_get_int("sys/BAT0/charge_control_start_threshold") == 55);
    CHECK(case_get_int("sys/BAT0/charge_control_end_threshold") == 85);

    free_fixture_model(&fm);
    return true;
}

/* ---- Test 3: Real ChromiumOS artifacts gate + live inference (optional env) ---- */

static bool copy_file_into(const char *src_path, const char *dst_path)
{
    FILE *in = fopen(src_path, "rb");
    if (!in) return false;
    FILE *out = fopen(dst_path, "wb");
    if (!out) {
        fclose(in);
        return false;
    }
    unsigned char buf[8192];
    size_t n;
    bool ok = true;
    while ((n = fread(buf, 1, sizeof buf, in)) > 0) {
        if (fwrite(buf, 1, n, out) != n) {
            ok = false;
            break;
        }
    }
    if (ferror(in)) ok = false;
    if (fclose(out) != 0) ok = false;
    fclose(in);
    return ok;
}

static void run_real_artifacts_test(void)
{
    const char *art_dir = getenv("LOCACHY_BATTERY_ARTIFACTS");
    if (!art_dir || !art_dir[0]) {
        printf("SKIP real ChromiumOS artifacts test (LOCACHY_BATTERY_ARTIFACTS unset)\n");
        return;
    }

    const int64_t t0 = 1769817600LL + 23 * 3600; /* 23:00 UTC overnight plug-in */
    setup_case("real-artifacts");
    create_single_pack(1, 50, "Charging", 50, 90);
    case_put_txt("rc.conf", "battery_charge_adaptive=\"1\"\n");
    case_mkd("hist/model");

    char src_tfl[1500], src_pb[1500], dst_tfl[1500], dst_pb[1500];
    snprintf(src_tfl, sizeof src_tfl, "%s/%s", art_dir, BAT_MODEL_TFLITE_NAME);
    snprintf(src_pb, sizeof src_pb, "%s/%s", art_dir, BAT_MODEL_PREPROC_NAME);
    snprintf(dst_tfl, sizeof dst_tfl, "%s/hist/model/%s", g_case, BAT_MODEL_TFLITE_NAME);
    snprintf(dst_pb, sizeof dst_pb, "%s/hist/model/%s", g_case, BAT_MODEL_PREPROC_NAME);

    bool ok = copy_file_into(src_tfl, dst_tfl) && copy_file_into(src_pb, dst_pb);
    if (!ok) {
        printf("FAIL real ChromiumOS artifacts: cannot copy from %s\n", art_dir);
        g_fail++;
        return;
    }

    /* Production pins must accept the real artifacts and load the real TFLite model. */
    active_model_pins = &prod_model_pins;
    write_history_for_pack("Lenovo_5B10W139_4242", 15, 8, 6, 9, t0, 500.0);
    set_clock(t0, 500.0);
    if (model_gate_check(1) != MODEL_GATE_READY || !adaptive_model) {
        printf("FAIL real ChromiumOS artifacts: production pin gate rejected real files\n");
        g_fail++;
        return;
    }
    if (record_snapshot(1) != 1) {
        printf("FAIL real ChromiumOS artifacts: record_snapshot failed\n");
        g_fail++;
        return;
    }
    struct bat_charge_slot_status ss;
    if (!read_slot_status("BAT0", &ss, NULL)) {
        printf("FAIL real ChromiumOS artifacts: missing charge-status\n");
        g_fail++;
        return;
    }
    /* Real model inference must succeed (hold or inactive depending on learned CDF,
     * never model-invalid, model-missing, learning, or error). */
    bool valid_state = strcmp(ss.state, "hold") == 0 || strcmp(ss.state, "inactive") == 0;
    if (!valid_state || adaptive_pred_failed) {
        printf("FAIL real ChromiumOS artifacts: unexpected state=%s pred_failed=%d\n",
               ss.state, adaptive_pred_failed);
        g_fail++;
        return;
    }

    /* Verify direct inference probabilities sum to ~1.0 on real 201 features. */
    struct bat_ctrl_context ctx;
    bat_ctrl_default_context(&ctx);
    ctx.local_hour = 23;
    ctx.day_of_week = 2;
    ctx.day_of_month = 15;
    struct bat_feature feats[BAT_CTRL_FEATURE_COUNT];
    double probs[BAT_ADAPTIVE_OUTPUTS] = {0};
    int nf = bat_ctrl_features(&adaptive_ctrl, &ctx, t0, 500.0, feats, BAT_CTRL_FEATURE_COUNT);
    double sum = 0.0;
    bool pred_ok = nf == BAT_CTRL_FEATURE_COUNT &&
                   bat_model_predict(adaptive_model, feats, (size_t)nf, probs) == 0;
    for (int i = 0; pred_ok && i < BAT_ADAPTIVE_OUTPUTS; i++) {
        if (!isfinite(probs[i]) || probs[i] < 0.0 || probs[i] > 1.0) pred_ok = false;
        sum += probs[i];
    }
    if (!pred_ok || fabs(sum - 1.0) > 1e-4) {
        printf("FAIL real ChromiumOS artifacts: inference output invalid (sum=%.6f)\n", sum);
        g_fail++;
        return;
    }
    adaptive_cleanup();
    printf("PASS real ChromiumOS artifacts: SHA-512 gate, TFLite load, and 201-feature inference (state=%s)\n",
           ss.state);
    g_pass++;
}

/* ---- Test 4: Hold thresholds (with/without start attr, pct > 80), bounded release -> top-up -> restore ---- */

static double g_override_probs[BAT_ADAPTIVE_OUTPUTS];
static int g_override_fail;
static int g_override_calls;

static int test_predict_cb(const struct bat_model *m, const struct bat_feature *feats,
                           size_t n, double out[BAT_ADAPTIVE_OUTPUTS])
{
    (void)m;
    (void)feats;
    g_override_calls++;
    if (n != BAT_CTRL_FEATURE_COUNT || g_override_fail)
        return -1;
    memcpy(out, g_override_probs, sizeof g_override_probs);
    return 0;
}

static bool test_hold_release_topup_and_thresholds(void)
{
    const int64_t t0 = 1769817600LL;
    setup_case("hold-release");
    create_single_pack(1, 60, "Charging", 55, 85);
    case_put_txt("rc.conf", "battery_charge_adaptive=\"1\"\n");
    struct fixture_model fm;
    install_fixture_model(&fm, 8);
    write_history_for_pack("Lenovo_5B10W139_4242", 15, 6, 4, 12, t0, 1000.0);

    /* Bucket 5 = 1.0 -> target 5 h, release_at = +3 h = boot 11800, wall t0 + 10800. */
    memset(g_override_probs, 0, sizeof g_override_probs);
    g_override_probs[5] = 1.0;
    g_override_fail = 0;
    g_override_calls = 0;
    bat_predict_override = test_predict_cb;

    set_clock(t0, 1000.0);
    CHECK(record_snapshot(1) == 1);
    struct bat_charge_slot_status ss;
    CHECK(read_slot_status("BAT0", &ss, NULL));
    CHECK(strcmp(ss.state, "hold") == 0);
    CHECK(ss.requested_end == 80 && ss.actual_end == 80);
    CHECK(ss.release_ts == t0 + 3 * 3600);
    CHECK(case_get_int("sys/BAT0/charge_control_start_threshold") == 75);
    CHECK(case_get_int("sys/BAT0/charge_control_end_threshold") == 80);
    CHECK(g_override_calls == 1);

    /* Advance past release alarm (+3 h) -> top-up (99/100) because we held on this plug. */
    set_clock(t0 + 3 * 3600, 1000.0 + 3.0 * 3600.0);
    CHECK(record_snapshot(0) == 1);
    CHECK(read_slot_status("BAT0", &ss, NULL));
    CHECK(strcmp(ss.state, "top-up") == 0);
    CHECK(ss.requested_end == 100 && ss.actual_end == 100);
    CHECK(ss.release_ts == 0);
    CHECK(case_get_int("sys/BAT0/charge_control_start_threshold") == 99);
    CHECK(case_get_int("sys/BAT0/charge_control_end_threshold") == 100);

    /* Unplug AC -> on-battery, restores saved baseline 55/85 and removes baseline marker. */
    case_put_txt("sys/AC/online", "0\n");
    case_put_txt("sys/BAT0/status", "Discharging\n");
    set_clock(t0 + 4 * 3600, 1000.0 + 4.0 * 3600.0);
    CHECK(record_snapshot(0) == 1);
    CHECK(read_slot_status("BAT0", &ss, NULL));
    CHECK(strcmp(ss.state, "on-battery") == 0);
    CHECK(case_get_int("sys/BAT0/charge_control_start_threshold") == 55);
    CHECK(case_get_int("sys/BAT0/charge_control_end_threshold") == 85);
    CHECK(!case_exists("hist/.charge-BAT0.Lenovo_5B10W139_4242"));

    /* Replug at 88% (> 80% hold_percent) with unbounded bucket 8 -> hold at 83/88, release_ts=0. */
    memset(g_override_probs, 0, sizeof g_override_probs);
    g_override_probs[8] = 1.0;
    case_put_txt("sys/AC/online", "1\n");
    case_put_txt("sys/BAT0/status", "Charging\n");
    case_put_txt("sys/BAT0/energy_now", "44000000\n"); /* 88% of 50 Wh */
    set_clock(t0 + 5 * 3600, 1000.0 + 5.0 * 3600.0);
    CHECK(record_snapshot(0) == 1);
    CHECK(read_slot_status("BAT0", &ss, NULL));
    CHECK(strcmp(ss.state, "hold") == 0);
    CHECK(ss.requested_end == 88 && ss.actual_end == 88);
    CHECK(ss.release_ts == 0);
    CHECK(case_get_int("sys/BAT0/charge_control_start_threshold") == 83);
    CHECK(case_get_int("sys/BAT0/charge_control_end_threshold") == 88);

    /* Unplug, drop start_threshold attr (end-only hardware), replug at 60%:
     * HOLD sets end=80 and never creates charge_control_start_threshold. */
    case_put_txt("sys/AC/online", "0\n");
    case_put_txt("sys/BAT0/status", "Discharging\n");
    set_clock(t0 + 6 * 3600, 1000.0 + 6.0 * 3600.0);
    CHECK(record_snapshot(0) == 1);
    CHECK(case_get_int("sys/BAT0/charge_control_end_threshold") == 85);

    case_unlink("sys/BAT0/charge_control_start_threshold");
    case_put_txt("sys/AC/online", "1\n");
    case_put_txt("sys/BAT0/status", "Charging\n");
    case_put_txt("sys/BAT0/energy_now", "30000000\n"); /* 60% */
    set_clock(t0 + 7 * 3600, 1000.0 + 7.0 * 3600.0);
    CHECK(record_snapshot(1) == 1);
    CHECK(read_slot_status("BAT0", &ss, NULL));
    CHECK(strcmp(ss.state, "hold") == 0);
    CHECK(ss.requested_end == 80 && ss.actual_end == 80);
    CHECK(case_get_int("sys/BAT0/charge_control_end_threshold") == 80);
    CHECK(!case_exists("sys/BAT0/charge_control_start_threshold"));

    free_fixture_model(&fm);
    return true;
}

/* ---- Test 5: Immediate RESUME without prior hold stays inactive & keeps manual/baseline ---- */

static bool test_inactive_without_hold_and_96_guard(void)
{
    const int64_t t0 = 1769817600LL;
    setup_case("inactive-no-hold");
    create_single_pack(1, 60, "Charging", 70, 90);
    case_put_txt("rc.conf", "battery_charge_adaptive=\"1\"\n"
                            "battery_charge_start=\"70\"\n"
                            "battery_charge_end=\"90\"\n");
    struct fixture_model fm;
    install_fixture_model(&fm, 0); /* bucket 0 -> immediate RESUME without ever holding */
    write_history_for_pack("Lenovo_5B10W139_4242", 15, 6, 4, 12, t0, 1000.0);

    set_clock(t0, 1000.0);
    CHECK(record_snapshot(1) == 1);
    struct bat_charge_slot_status ss;
    CHECK(read_slot_status("BAT0", &ss, NULL));
    CHECK(strcmp(ss.state, "inactive") == 0);
    /* Must NOT write 99/100 since pack was never held on this plug. */
    CHECK(case_get_int("sys/BAT0/charge_control_start_threshold") == 70);
    CHECK(case_get_int("sys/BAT0/charge_control_end_threshold") == 90);
    CHECK(!case_exists("hist/.charge-BAT0.Lenovo_5B10W139_4242"));

    /* Unplug and replug at 97% (above 96% guard) -> inactive, no hold. */
    case_put_txt("sys/AC/online", "0\n");
    case_put_txt("sys/BAT0/status", "Discharging\n");
    set_clock(t0 + 60, 1060.0);
    CHECK(record_snapshot(0) == 1);

    memset(g_override_probs, 0, sizeof g_override_probs);
    g_override_probs[8] = 1.0;
    g_override_fail = 0;
    g_override_calls = 0;
    bat_predict_override = test_predict_cb;
    case_put_txt("sys/AC/online", "1\n");
    case_put_txt("sys/BAT0/status", "Charging\n");
    case_put_txt("sys/BAT0/energy_now", "48500000\n"); /* 97% */
    set_clock(t0 + 120, 1120.0);
    CHECK(record_snapshot(0) == 1);
    CHECK(read_slot_status("BAT0", &ss, NULL));
    CHECK(strcmp(ss.state, "inactive") == 0);
    CHECK(g_override_calls == 0);
    CHECK(case_get_int("sys/BAT0/charge_control_start_threshold") == 70);
    CHECK(case_get_int("sys/BAT0/charge_control_end_threshold") == 90);

    free_fixture_model(&fm);
    return true;
}

/* ---- Test 6: Prediction failure -> controller stop path -> baseline restored (not hold, not top-up) ---- */

static bool test_prediction_failure_falls_back(void)
{
    const int64_t t0 = 1769817600LL;
    setup_case("pred-fail");
    create_single_pack(1, 60, "Charging", 55, 85);
    case_put_txt("rc.conf", "battery_charge_adaptive=\"1\"\n");
    struct fixture_model fm;
    install_fixture_model(&fm, 8);
    write_history_for_pack("Lenovo_5B10W139_4242", 15, 6, 4, 12, t0, 1000.0);

    /* Start holding first. */
    memset(g_override_probs, 0, sizeof g_override_probs);
    g_override_probs[8] = 1.0;
    g_override_fail = 0;
    bat_predict_override = test_predict_cb;
    set_clock(t0, 1000.0);
    CHECK(record_snapshot(1) == 1);
    struct bat_charge_slot_status ss;
    CHECK(read_slot_status("BAT0", &ss, NULL));
    CHECK(strcmp(ss.state, "hold") == 0);
    CHECK(case_get_int("sys/BAT0/charge_control_start_threshold") == 75);
    CHECK(case_get_int("sys/BAT0/charge_control_end_threshold") == 80);

    /* Recheck after 30 min fails prediction -> controller stops, restores 55/85, state inactive. */
    g_override_fail = 1;
    set_clock(t0 + 1800, 1000.0 + 1800.0);
    CHECK(record_snapshot(0) == 1);
    CHECK(read_slot_status("BAT0", &ss, NULL));
    CHECK(strcmp(ss.state, "inactive") == 0);
    CHECK(case_get_int("sys/BAT0/charge_control_start_threshold") == 55);
    CHECK(case_get_int("sys/BAT0/charge_control_end_threshold") == 85);
    CHECK(!case_exists("hist/.charge-BAT0.Lenovo_5B10W139_4242"));

    /* Fresh plug-in where initial prediction fails immediately -> never holds, stays 55/85. */
    case_put_txt("sys/AC/online", "0\n");
    case_put_txt("sys/BAT0/status", "Discharging\n");
    set_clock(t0 + 2000, 1000.0 + 2000.0);
    CHECK(record_snapshot(0) == 1);
    case_put_txt("sys/AC/online", "1\n");
    case_put_txt("sys/BAT0/status", "Charging\n");
    set_clock(t0 + 2100, 1000.0 + 2100.0);
    CHECK(record_snapshot(0) == 1);
    CHECK(read_slot_status("BAT0", &ss, NULL));
    CHECK(strcmp(ss.state, "inactive") == 0);
    CHECK(case_get_int("sys/BAT0/charge_control_start_threshold") == 55);
    CHECK(case_get_int("sys/BAT0/charge_control_end_threshold") == 85);

    free_fixture_model(&fm);
    return true;
}

/* ---- Test 7: Charge to Full Now while holding -> USER_CANCELED, full, expiry, unplug ---- */

static bool test_charge_to_full_now_during_hold(void)
{
    const int64_t t0 = 1769817600LL;
    setup_case("charge-now");
    create_single_pack(1, 60, "Charging", 55, 85);
    case_put_txt("rc.conf", "battery_charge_adaptive=\"1\"\n");
    struct fixture_model fm;
    install_fixture_model(&fm, 8);
    write_history_for_pack("Lenovo_5B10W139_4242", 15, 6, 4, 12, t0, 1000.0);

    set_clock(t0, 1000.0);
    CHECK(record_snapshot(1) == 1);
    struct bat_charge_slot_status ss;
    CHECK(read_slot_status("BAT0", &ss, NULL));
    CHECK(strcmp(ss.state, "hold") == 0);
    CHECK(case_get_int("sys/BAT0/charge_control_end_threshold") == 80);

    /* User invokes Charge to Full Now -> controller enters USER_CANCELED, thresholds 99/100. */
    char rc[128];
    int64_t until = t0 + 3600;
    snprintf(rc, sizeof rc, "battery_charge_adaptive=\"1\"\nbattery_charge_full_until=\"%lld\"\n",
             (long long)until);
    case_put_txt("rc.conf", rc);
    set_clock(t0 + 60, 1060.0);
    CHECK(record_snapshot(1) == 1);
    struct bat_charge_status full_st;
    CHECK(read_slot_status("BAT0", &ss, &full_st));
    CHECK(strcmp(ss.state, "full") == 0);
    CHECK(full_st.full_until == until);
    CHECK(adaptive_ctrl.state == BAT_CTRL_STATE_USER_CANCELED);
    CHECK(case_get_int("sys/BAT0/charge_control_start_threshold") == 99);
    CHECK(case_get_int("sys/BAT0/charge_control_end_threshold") == 100);

    /* After deadline expires while still plugged -> USER_CANCELED stays inactive (no re-hold),
     * baseline 55/85 restored. */
    set_clock(until, 1000.0 + 3600.0);
    CHECK(record_snapshot(0) == 1);
    CHECK(read_slot_status("BAT0", &ss, &full_st));
    CHECK(strcmp(ss.state, "inactive") == 0);
    CHECK(full_st.full_until == 0);
    CHECK(case_get_int("sys/BAT0/charge_control_start_threshold") == 55);
    CHECK(case_get_int("sys/BAT0/charge_control_end_threshold") == 85);

    /* Next unplug + replug clears USER_CANCELED and resumes holding. */
    case_put_txt("sys/AC/online", "0\n");
    case_put_txt("sys/BAT0/status", "Discharging\n");
    set_clock(until + 60, 1000.0 + 3660.0);
    CHECK(record_snapshot(0) == 1);
    case_put_txt("sys/AC/online", "1\n");
    case_put_txt("sys/BAT0/status", "Charging\n");
    set_clock(until + 120, 1000.0 + 3720.0);
    CHECK(record_snapshot(0) == 1);
    CHECK(read_slot_status("BAT0", &ss, NULL));
    CHECK(strcmp(ss.state, "hold") == 0);
    CHECK(case_get_int("sys/BAT0/charge_control_end_threshold") == 80);

    free_fixture_model(&fm);
    return true;
}

/* ---- Test 8: Remaining states (manual, unsupported, error, opt-out isolation) ---- */

static bool test_manual_unsupported_error_and_optout(void)
{
    const int64_t t0 = 1769817600LL;
    setup_case("states-optout");
    create_single_pack(1, 60, "Charging", 40, 85);
    case_put_txt("rc.conf", "battery_charge_adaptive=\"0\"\n"
                            "battery_charge_start=\"75\"\n"
                            "battery_charge_end=\"80\"\n");
    set_clock(t0, 100.0);

    /* Opted out: applies manual limits, publishes "manual", creates NO charge-history-*.bin. */
    CHECK(record_snapshot(1) == 1);
    struct bat_charge_slot_status ss;
    CHECK(read_slot_status("BAT0", &ss, NULL));
    CHECK(strcmp(ss.state, "manual") == 0);
    CHECK(case_get_int("sys/BAT0/charge_control_start_threshold") == 75);
    CHECK(case_get_int("sys/BAT0/charge_control_end_threshold") == 80);
    CHECK(!case_exists("hist/charge-history-Lenovo_5B10W139_4242.bin"));

    /* Missing end threshold (e.g. HP OMEN / unsupported EC) -> unsupported, no writes. */
    case_unlink("sys/BAT0/charge_control_end_threshold");
    case_put_txt("rc.conf", "battery_charge_adaptive=\"1\"\n");
    set_clock(t0 + 60, 160.0);
    CHECK(record_snapshot(1) == 1);
    CHECK(read_slot_status("BAT0", &ss, NULL));
    CHECK(strcmp(ss.state, "unsupported") == 0);
    CHECK(!case_exists("sys/BAT0/charge_control_end_threshold"));

    /* Unreadable end threshold -> error. */
    case_put_txt("sys/BAT0/charge_control_end_threshold", "nan\n");
    set_clock(t0 + 120, 220.0);
    CHECK(record_snapshot(1) == 1);
    CHECK(read_slot_status("BAT0", &ss, NULL));
    CHECK(strcmp(ss.state, "error") == 0);

    return true;
}

/* ---- Test 9: ChargeHistory persistence (0600 atomic save, reload, corrupt rejection) ---- */

static bool test_history_persistence_and_corruption(void)
{
    const int64_t t0 = 1769817600LL;
    setup_case("hist-persist");
    create_single_pack(1, 60, "Charging", 55, 85);
    case_put_txt("rc.conf", "battery_charge_adaptive=\"1\"\n");
    struct fixture_model fm;
    install_fixture_model(&fm, 8);

    /* Start empty, accumulate a session and verify 0600 file is saved. */
    set_clock(t0, 100.0);
    CHECK(record_snapshot(1) == 1);
    char hpath[1500];
    snprintf(hpath, sizeof hpath, "%s/hist/charge-history-Lenovo_5B10W139_4242.bin", g_case);
    struct stat st;
    CHECK(stat(hpath, &st) == 0);
    CHECK((st.st_mode & 0777) == 0600);

    /* Unplug after 1 hour -> completed charge event persisted and reloaded on restart. */
    case_put_txt("sys/AC/online", "0\n");
    case_put_txt("sys/BAT0/status", "Discharging\n");
    set_clock(t0 + 3600, 3700.0);
    CHECK(record_snapshot(0) == 1);
    CHECK(adaptive_ctrl.history.event_count == 1);
    CHECK(adaptive_ctrl.history.events[0].duration_sec == 3600);

    /* Simulate restart: clear memory, reload from disk. */
    reset_adaptive_controller(0);
    memset(slots, 0, sizeof slots);
    nslots = 0;
    set_clock(t0 + 3660, 3760.0);
    CHECK(record_snapshot(1) == 1);
    CHECK(adaptive_ctrl_ready);
    CHECK(adaptive_ctrl.history.event_count == 1);
    CHECK(adaptive_ctrl.history.events[0].duration_sec == 3600);

    /* Corrupt persisted file -> daemon starts cleanly with empty history (no crash). */
    reset_adaptive_controller(0);
    memset(slots, 0, sizeof slots);
    nslots = 0;
    int64_t bad[5] = { 999, 0, 0, 0, 0 }; /* wrong serialization version */
    case_put_bin("hist/charge-history-Lenovo_5B10W139_4242.bin", bad, sizeof bad);
    set_clock(t0 + 3720, 3820.0);
    CHECK(record_snapshot(1) == 1);
    CHECK(adaptive_ctrl_ready);
    CHECK(adaptive_ctrl.history.event_count == 0);

    /* Shutdown while holding restores baseline thresholds cleanly. */
    write_history_for_pack("Lenovo_5B10W139_4242", 15, 6, 4, 12, t0 + 4000, 4100.0);
    reset_adaptive_controller(0);
    case_put_txt("sys/AC/online", "1\n");
    case_put_txt("sys/BAT0/status", "Charging\n");
    set_clock(t0 + 4000, 4100.0);
    CHECK(record_snapshot(1) == 1);
    CHECK(case_get_int("sys/BAT0/charge_control_end_threshold") == 80);
    handle_shutdown();
    CHECK(case_get_int("sys/BAT0/charge_control_start_threshold") == 55);
    CHECK(case_get_int("sys/BAT0/charge_control_end_threshold") == 85);

    free_fixture_model(&fm);
    return true;
}

int main(void)
{
    char tmpl[] = "/tmp/test-battery-adaptive-XXXXXX";
    char *dir = mkdtemp(tmpl);
    if (!dir) {
        fprintf(stderr, "FAIL mkdtemp: %s\n", strerror(errno));
        return 1;
    }
    snprintf(g_root, sizeof g_root, "%s", dir);

    run_test("manifest pin consistency: battery-model-pins.h matches battery-adaptive-manifest.json",
             test_manifest_pin_consistency);
    run_test("model gate: missing, wrong size, wrong hash, symlink, parse fail, and valid fixture",
             test_model_gate_fixtures);
    run_real_artifacts_test();
    run_test("hold/release/top-up thresholds: max(hold,pct), start=end-5, end-only pack, and baseline restore",
             test_hold_release_topup_and_thresholds);
    run_test("inactive without prior hold and 96% guard keep manual/baseline limits",
             test_inactive_without_hold_and_96_guard);
    run_test("prediction failure stops adaptive charging and restores baseline limits",
             test_prediction_failure_falls_back);
    run_test("Charge to Full Now maps to USER_CANCELED, restores on expiry, and re-arms on replug",
             test_charge_to_full_now_during_hold);
    run_test("status states: manual (opt-out isolation), unsupported, and error",
             test_manual_unsupported_error_and_optout);
    run_test("ChargeHistory persistence: 0600 atomic write, reload, corrupt rejection, and shutdown restore",
             test_history_persistence_and_corruption);

    adaptive_cleanup();
    rm_rf_dir(g_root);
    return g_fail == 0 ? 0 : 1;
}
