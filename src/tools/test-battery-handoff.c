/*
 * Removable-battery pack identity & handoff regression test:
 *   1. Per-pack identity (.tsv + .meta) separate from slot name ("BAT0")
 *   2. Hot-remove on AC -> 'R' break sample + REMOVE event + offline spare retention
 *   3. Hot-insert of another pack -> INSERT event + automatic EC-safe threshold apply
 *   4. Cold-swap across hibernate/restart -> SWAP handoff event + outgoing pack marked 'R'
 *   5. Dual-battery active discharge handoff (BAT1 -> BAT0) -> DISCHARGE event
 *   6. Adaptive charge handoff: model gate, unsupported/no-write, readback,
 *      per-pack baseline restore, one-time full expiry, restart persistence, status
 *   7. Software charge_behaviour single-pack swap: per-pack .charge-* baseline
 *      isolation and hysteresis when a different pack is inserted
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* Emulate kernel sysfs charge_behaviour readback formatting on plain fixture
 * files after bat_write_charge_behaviour() performs the real write. */
static void test_emulate_kernel_behaviour_write(const char *path, const char *mode)
{
    FILE *f = fopen(path, "w");
    if (!f) return;
    if (strcmp(mode, "auto") == 0) fprintf(f, "[auto] inhibit-charge\n");
    else if (strcmp(mode, "inhibit-charge") == 0) fprintf(f, "auto [inhibit-charge]\n");
    else fprintf(f, "%s\n", mode);
    fclose(f);
}

#define BAT_CHARGE_BEHAVIOUR_AFTER_WRITE(path, mode) test_emulate_kernel_behaviour_write((path), (mode))

#define main battery_history_main
#include "../progs/battery-history.c"
#undef main

static char root[1024];

static void mkd(const char *rel)
{
    char full[1280], cur[1280];
    snprintf(full, sizeof full, "%s/%s", root, rel);
    size_t len = strlen(full);
    for (size_t i = 1; i <= len; i++) {
        if (full[i] == '/' || full[i] == '\0') {
            memcpy(cur, full, i);
            cur[i] = '\0';
            (void)mkdir(cur, 0755);
        }
    }
}

static void put_txt(const char *rel, const char *txt)
{
    char p[1280];
    snprintf(p, sizeof p, "%s/%s", root, rel);
    FILE *f = fopen(p, "w");
    assert(f && fputs(txt, f) >= 0 && fclose(f) == 0);
}

static void put_bin(const char *rel, const void *buf, size_t len)
{
    char p[1280];
    snprintf(p, sizeof p, "%s/%s", root, rel);
    FILE *f = fopen(p, "wb");
    assert(f && fwrite(buf, 1, len, f) == len && fclose(f) == 0);
}

static int get_int(const char *rel)
{
    char p[1280], b[64] = "";
    snprintf(p, sizeof p, "%s/%s", root, rel);
    FILE *f = fopen(p, "r");
    assert(f && fgets(b, sizeof b, f));
    fclose(f);
    return atoi(b);
}

static void set_now(int64_t ts)
{
    char b[32];
    snprintf(b, sizeof b, "%lld", (long long)ts);
    assert(setenv("LOCACHY_NOW_TS", b, 1) == 0);
}

static void reset_daemon_memory(void)
{
    adaptive_cleanup();
    memset(slots, 0, sizeof slots);
    last_ac_online = 0;
    last_system_ac = BAT_AC_UNKNOWN;
    cancelled_full_until = 0;
    intent_loaded = intent_error = 0;
    nslots = 0;
    any_prior_pack = 0;
    active_dis_slot[0] = '\0';
    active_dis_pack[0] = '\0';
    active_dis_dpct = 0;
}

static struct bat_charge_slot_status charge_status_for(const char *slot, const char *state,
                                                        int requested, int actual, int64_t full_until)
{
    struct bat_charge_status status;
    assert(bat_read_charge_status(&status) == 0 && status.full_until == full_until);
    for (int i = 0; i < status.nslots; i++) {
        struct bat_charge_slot_status *s = &status.slots[i];
        if (!strcmp(s->slot, slot)) {
            assert(!strcmp(s->state, state) && s->requested_end == requested && s->actual_end == actual);
            return *s;
        }
    }
    assert(!"missing charge status slot");
    abort();
}

/* ---- Minimal valid TFLite + ExamplePreprocessor fixture for handoff tests ---- */

struct fb_vec_w {
    uint8_t *buf;
    size_t len, cap;
};

static void fbw_ensure(struct fb_vec_w *w, size_t extra)
{
    if (w->len + extra <= w->cap) return;
    size_t ncap = w->cap ? w->cap * 2 : 512;
    while (ncap < w->len + extra) ncap *= 2;
    w->buf = realloc(w->buf, ncap);
    assert(w->buf != NULL);
    w->cap = ncap;
}

static void fbw_bytes(struct fb_vec_w *w, const void *p, size_t n)
{
    fbw_ensure(w, n);
    memcpy(w->buf + w->len, p, n);
    w->len += n;
}

static void fbw_u8(struct fb_vec_w *w, uint8_t v) { fbw_bytes(w, &v, 1); }

static void fbw_u16(struct fb_vec_w *w, uint16_t v)
{
    uint8_t b[2] = { (uint8_t)v, (uint8_t)(v >> 8) };
    fbw_bytes(w, b, 2);
}

static void fbw_u32(struct fb_vec_w *w, uint32_t v)
{
    uint8_t b[4] = { (uint8_t)v, (uint8_t)(v >> 8), (uint8_t)(v >> 16), (uint8_t)(v >> 24) };
    fbw_bytes(w, b, 4);
}

static void fbw_i32(struct fb_vec_w *w, int32_t v)
{
    uint32_t u;
    memcpy(&u, &v, 4);
    fbw_u32(w, u);
}

static void fbw_f32(struct fb_vec_w *w, float v)
{
    uint32_t u;
    memcpy(&u, &v, 4);
    fbw_u32(w, u);
}

static void fbw_align4(struct fb_vec_w *w)
{
    while (w->len & 3u) fbw_u8(w, 0);
}

static void fbw_patch_u32(struct fb_vec_w *w, size_t pos, uint32_t v)
{
    w->buf[pos + 0] = (uint8_t)v;
    w->buf[pos + 1] = (uint8_t)(v >> 8);
    w->buf[pos + 2] = (uint8_t)(v >> 16);
    w->buf[pos + 3] = (uint8_t)(v >> 24);
}

/* Build a minimal 1-layer FC(1 -> 9) + SOFTMAX TFLite FlatBuffer whose bias
 * pushes bucket 8 to probability ~1.0 (unbounded hold -> HOLD at 80%). */
static uint8_t *build_fixture_tflite(size_t *out_len)
{
    struct fb_vec_w w = {0};

    /* Forward-only layout: reserve tables first, then emit referenced payloads. */
    fbw_u32(&w, 0);
    fbw_bytes(&w, "TFL3", 4);

    /* Model vtable + table */
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

    /* operator_codes vector (2) */
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

    /* buffers vector (3): buf0 empty, buf1 weights [9,1], buf2 bias [9] */
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
    for (int i = 0; i < 8; i++) fbw_f32(&w, -10.0f);
    fbw_f32(&w, 10.0f); /* bucket 8 -> ~1.0 */

    /* subgraphs vector (1) */
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

    /* inputs=[0], outputs=[4] */
    size_t inv = w.len;
    fbw_patch_u32(&w, sg_in, (uint32_t)(inv - sg_in));
    fbw_u32(&w, 1); fbw_i32(&w, 0);
    size_t outv = w.len;
    fbw_patch_u32(&w, sg_out, (uint32_t)(outv - sg_out));
    fbw_u32(&w, 1); fbw_i32(&w, 4);

    /* 5 tensors: 0:in[1,1], 1:w[9,1](buf1), 2:b[9](buf2), 3:logits[1,9], 4:probs[1,9] */
    size_t tv = w.len;
    fbw_patch_u32(&w, sg_t, (uint32_t)(tv - sg_t));
    fbw_u32(&w, 5);
    size_t tv_s[5];
    for (int i = 0; i < 5; i++) { tv_s[i] = w.len; fbw_u32(&w, 0); }
    size_t tvt = w.len;
    fbw_u16(&w, 10); fbw_u16(&w, 12);
    fbw_u16(&w, 4); fbw_u16(&w, 0); fbw_u16(&w, 8); /* shape@4, type default 0, buffer@8 */
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

    /* 2 operators: op0 FC(0,1,2 -> 3), op1 SOFTMAX(3 -> 4, beta=1.0) */
    size_t opv = w.len;
    fbw_patch_u32(&w, sg_ops, (uint32_t)(opv - sg_ops));
    fbw_u32(&w, 2);
    size_t opv0 = w.len; fbw_u32(&w, 0);
    size_t opv1 = w.len; fbw_u32(&w, 0);

    size_t opvt_fc = w.len;
    fbw_u16(&w, 10); fbw_u16(&w, 16);
    fbw_u16(&w, 4); fbw_u16(&w, 8); fbw_u16(&w, 12); /* opcode_index, inputs, outputs */
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
    fbw_u8(&w, 9); fbw_u8(&w, 0); fbw_u8(&w, 0); fbw_u8(&w, 0); /* TFL_OPTS_SOFTMAX */
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

/* ExamplePreprocessorConfig with one feature_indices entry: "IsCharging" -> 0. */
static uint8_t *build_fixture_preproc(size_t *out_len)
{
    static const uint8_t pb[] = {
        0x1a, 0x0e,             /* field 3 (feature_indices), len 14 */
        0x0a, 0x0a,             /* key "IsCharging" (len 10) */
        'I', 's', 'C', 'h', 'a', 'r', 'g', 'i', 'n', 'g',
        0x10, 0x00              /* value 0 */
    };
    uint8_t *copy = malloc(sizeof pb);
    assert(copy != NULL);
    memcpy(copy, pb, sizeof pb);
    *out_len = sizeof pb;
    return copy;
}

static void sha512_hex(const uint8_t *buf, size_t len, char hex[129])
{
    unsigned char md[EVP_MAX_MD_SIZE];
    unsigned int md_len = 0;
    EVP_MD_CTX *ctx = EVP_MD_CTX_new();
    assert(ctx && EVP_DigestInit_ex(ctx, EVP_sha512(), NULL) == 1 &&
           EVP_DigestUpdate(ctx, buf, len) == 1 &&
           EVP_DigestFinal_ex(ctx, md, &md_len) == 1 && md_len == 64);
    EVP_MD_CTX_free(ctx);
    for (int i = 0; i < 64; i++)
        snprintf(hex + i * 2, 3, "%02x", md[i]);
    hex[128] = '\0';
}

/* Write a valid serialized 14-day ChargeHistory (ratio 0.75 >= 0.5) for pack_id. */
static void write_mature_ctrl_history(const char *pack_id, int64_t now_ts)
{
    struct bat_ctrl tmp;
    assert(bat_ctrl_init(&tmp, NULL) == 0);
    struct bat_history_snapshot snap;
    memset(&snap, 0, sizeof snap);
    int64_t today = bat_hist_utc_midnight(now_ts);
    for (int i = 0; i < 15; i++) {
        snap.days[snap.day_count++] = (struct bat_daily_summary){
            .day_utc_sec = today - (int64_t)(14 - i) * BAT_CTRL_DAY_SEC,
            .time_on_ac_sec = 4 * 3600,
            .time_full_on_ac_sec = 3 * 3600,
            .hold_time_on_ac_sec = 0,
        };
    }
    for (int i = 0; i < 10; i++) {
        snap.events[snap.event_count++] = (struct bat_charge_event){
            .start_wall_sec = today - (int64_t)(10 - i) * 12 * 3600,
            .duration_sec = 10 * 3600,
        };
    }
    struct bat_ctrl_power off = { false, false, 60.0, 60.0 };
    assert(bat_ctrl_import_history(&tmp, &snap, &off, now_ts, current_boot_sec(now_ts)) == 0);

    int64_t buf[BAT_CTRL_SERIAL_MAX_I64];
    int n = bat_ctrl_serialize_history(&tmp, buf, BAT_CTRL_SERIAL_MAX_I64);
    assert(n > 0);
    char rel[256];
    snprintf(rel, sizeof rel, "hist/charge-history-%s.bin", pack_id);
    put_bin(rel, buf, (size_t)n * sizeof(int64_t));
}

static void test_adaptive(int64_t t0)
{
    const char *start = "sys/BAT0/charge_control_start_threshold";
    const char *end = "sys/BAT0/charge_control_end_threshold";
    const char *ac = "sys/AC/online";
    char expiry[96], end_path[1280];
    snprintf(end_path, sizeof end_path, "%s/%s", root, end);
    put_txt(ac, "1\n");
    put_txt("sys/BAT0/status", "Not charging\n");
    put_txt("sys/BAT0/energy_now", "33000000\n"); /* 60% of 55 Wh */
    put_txt("sys/BAT1/status", "Not charging\n");
    put_txt("sys/BAT1/charge_control_start_threshold", "30\n");
    put_txt("sys/BAT1/charge_behaviour", "[auto] inhibit-charge force-discharge\n");
    put_txt(start, "63\n");
    put_txt(end, "87\n");
    put_txt("rc.conf", "battery_charge_adaptive=\"1\"\n");
    set_now(t0);
    assert(record_snapshot(0) == 2);
    charge_status_for("BAT0", "unsupported", -1, 87, 0);
    charge_status_for("BAT1", "unsupported", -1, -1, 0);
    assert(get_int(start) == 63 && get_int(end) == 87);
    assert(get_int("sys/BAT1/charge_control_start_threshold") == 30);

    /* Single supported pack with adaptive opted in but no model files -> model-missing. */
    put_txt("sys/BAT1/present", "0\n");
    assert(record_snapshot(0) == 1);
    charge_status_for("BAT0", "model-missing", -1, 87, 0);
    assert(get_int(start) == 63 && get_int(end) == 87);

    /* Install fixture model and pin it for the remainder of test_adaptive(). */
    size_t tfl_len = 0, pb_len = 0;
    uint8_t *tfl = build_fixture_tflite(&tfl_len);
    uint8_t *pb = build_fixture_preproc(&pb_len);
    char tfl_sha[129], pb_sha[129];
    sha512_hex(tfl, tfl_len, tfl_sha);
    sha512_hex(pb, pb_len, pb_sha);
    struct bat_model_pin_spec fixture_pins = {
        .tflite_name    = BAT_MODEL_TFLITE_NAME,
        .tflite_size    = tfl_len,
        .tflite_sha512  = tfl_sha,
        .preproc_name   = BAT_MODEL_PREPROC_NAME,
        .preproc_size   = pb_len,
        .preproc_sha512 = pb_sha,
    };
    active_model_pins = &fixture_pins;
    mkd("hist/model");
    put_bin("hist/model/" BAT_MODEL_TFLITE_NAME, tfl, tfl_len);
    put_bin("hist/model/" BAT_MODEL_PREPROC_NAME, pb, pb_len);

    /* Fresh pack with verified model -> learning (< 14 days history), no threshold write. */
    set_now(t0 + 30);
    assert(record_snapshot(1) == 1);
    charge_status_for("BAT0", "learning", -1, 87, 0);
    assert(get_int(start) == 63 && get_int(end) == 87);

    /* Repeated snapshots must not write unchanged threshold values. */
    struct timespec stamps[2] = {{ .tv_sec = 100 }, { .tv_sec = 100 }};
    assert(utimensat(AT_FDCWD, end_path, stamps, 0) == 0);
    set_now(t0 + 60);
    assert(record_snapshot(0) == 1);
    struct stat st;
    assert(stat(end_path, &st) == 0 && st.st_mtim.tv_sec == 100);

    /* Supply mature 14-day ChargeHistory for Panasonic_CF-VZSU1QJS_9001 -> HOLD at 75..80. */
    reset_daemon_memory();
    load_previous_slots();
    write_mature_ctrl_history("Panasonic_CF-VZSU1QJS_9001", t0 + 120);
    set_now(t0 + 120);
    assert(record_snapshot(1) == 1);
    struct bat_charge_slot_status hold_st = charge_status_for("BAT0", "hold", 80, 80, 0);
    assert(hold_st.release_ts == 0); /* fixture outputs bucket 8 -> unbounded hold */
    assert(get_int(start) == 75 && get_int(end) == 80);

    /* Restart while adapted; an unrelated pack must not receive this baseline. */
    reset_daemon_memory();
    load_previous_slots();
    put_txt("sys/BAT0/serial_number", "9002\n");
    put_txt(start, "42\n");
    put_txt(end, "73\n");
    put_txt("rc.conf", "battery_charge_adaptive=\"0\"\n");
    assert(record_snapshot(1) == 1);
    charge_status_for("BAT0", "manual", -1, 73, 0);
    assert(get_int(start) == 42 && get_int(end) == 73);
    put_txt("sys/BAT0/serial_number", "9001\n");
    put_txt(start, "75\n");
    put_txt(end, "80\n");
    assert(record_snapshot(0) == 1);
    charge_status_for("BAT0", "manual", -1, 87, 0);
    assert(get_int(start) == 63 && get_int(end) == 87);

    /* One-time full while manual, cancel on unplug and keep it cancelled across restart. */
    int64_t until = t0 + 1200;
    snprintf(expiry, sizeof expiry, "battery_charge_full_until=\"%lld\"\n", (long long)until);
    put_txt("rc.conf", expiry);
    set_now(t0 + 180);
    assert(record_snapshot(1) == 1);
    charge_status_for("BAT0", "full", 100, 100, until);
    put_txt(ac, "0\n");
    assert(record_snapshot(0) == 1);
    charge_status_for("BAT0", "manual", -1, 87, 0);
    reset_daemon_memory();
    load_previous_slots();
    put_txt(ac, "1\n");
    assert(record_snapshot(1) == 1);
    charge_status_for("BAT0", "manual", -1, 87, 0);

    /* An explicit newer request activates; an expired request restores once. */
    until++;
    snprintf(expiry, sizeof expiry, "battery_charge_full_until=\"%lld\"\n", (long long)until);
    put_txt("rc.conf", expiry);
    assert(record_snapshot(1) == 1);
    charge_status_for("BAT0", "full", 100, 100, until);
    set_now(until);
    assert(record_snapshot(0) == 1);
    charge_status_for("BAT0", "manual", -1, 87, 0);
    assert(get_int(start) == 63 && get_int(end) == 87);

    /* Present but unreadable threshold is an error, not unsupported or success. */
    put_txt(end, "invalid\n");
    put_txt("rc.conf", "battery_charge_adaptive=\"1\"\n");
    assert(record_snapshot(1) == 1);
    charge_status_for("BAT0", "error", -1, -1, 0);
    assert(get_int(start) == 63 && get_int("sys/BAT1/charge_control_start_threshold") == 30);
    assert(bat_read_line(bat_supply_root(), "BAT1", "charge_behaviour", expiry, sizeof expiry) == 0 &&
           !strcmp(expiry, "[auto] inhibit-charge force-discharge"));

    /* Restore production pins and clean up heap fixtures. */
    active_model_pins = &prod_model_pins;
    adaptive_cleanup();
    free(tfl);
    free(pb);
}

/* Single pack with software charge_behaviour only: a swap hands the cap to the
 * incoming pack and never applies the outgoing pack's .charge-* baseline to it. */
static int rel_exists(const char *rel)
{
    char p[1280];
    snprintf(p, sizeof p, "%s/%s", root, rel);
    return access(p, F_OK) == 0;
}

static void drop(const char *rel)
{
    char p[1280];
    snprintf(p, sizeof p, "%s/%s", root, rel);
    (void)unlink(p);
}

static void test_sw_single_pack_swap(int64_t t0)
{
    const char *behav = "sys/BAT0/charge_behaviour";
    const char *old_bl = "hist/.charge-BAT0.Panasonic_CF-VZSU1QJS_9001";
    const char *new_bl = "hist/.charge-BAT0.SANYO_45N1029_2222";
    char line[64];

    /* Only BAT0 is present, with no charge threshold attributes. */
    put_txt("rc.conf", "battery_charge_adaptive=\"0\"\nbattery_charge_start=\"75\"\nbattery_charge_end=\"80\"\n");
    put_txt("sys/BAT1/present", "0\n");
    drop("sys/BAT0/charge_control_start_threshold");
    drop("sys/BAT0/charge_control_end_threshold");
    put_txt("sys/AC/online", "1\n");
    put_txt("sys/BAT0/manufacturer", "Panasonic\n");
    put_txt("sys/BAT0/model_name", "CF-VZSU1QJS\n");
    put_txt("sys/BAT0/serial_number", "9001\n");
    put_txt("sys/BAT0/status", "Charging\n");
    put_txt("sys/BAT0/energy_now", "52000000\n"); /* ~95% >= 80% */
    put_txt("sys/BAT0/energy_full", "55000000\n");
    put_txt("sys/BAT0/energy_full_design", "55000000\n");
    put_txt(behav, "[auto] inhibit-charge\n");
    reset_daemon_memory();

    /* 95% on AC reaches the end limit: inhibit-charge with the old pack's baseline recorded. */
    set_now(t0);
    assert(record_snapshot(0) == 1);
    charge_status_for("BAT0", "manual", 80, 80, 0);
    assert(bat_read_line(bat_supply_root(), "BAT0", "charge_behaviour", line, sizeof line) == 0 &&
           strcmp(line, "auto [inhibit-charge]") == 0);
    assert(rel_exists(old_bl));

    /* Swap in SANYO #2222 at 50%: its own charge is released to auto, and the old baseline is left alone. */
    put_txt("sys/BAT0/manufacturer", "SANYO\n");
    put_txt("sys/BAT0/model_name", "45N1029\n");
    put_txt("sys/BAT0/serial_number", "2222\n");
    put_txt("sys/BAT0/status", "Charging\n");
    put_txt("sys/BAT0/energy_now", "30000000\n"); /* 50% <= 75% */
    put_txt("sys/BAT0/energy_full", "60000000\n");
    put_txt("sys/BAT0/energy_full_design", "60000000\n");
    set_now(t0 + 60);
    assert(record_snapshot(0) == 1);
    charge_status_for("BAT0", "manual", 80, 80, 0);
    assert(bat_read_line(bat_supply_root(), "BAT0", "charge_behaviour", line, sizeof line) == 0 &&
           strcmp(line, "[auto] inhibit-charge") == 0);
    assert(rel_exists(old_bl) && !rel_exists(new_bl));

    /* SANYO #2222 reaches 85%: inhibits and writes its own baseline independently. */
    put_txt("sys/BAT0/energy_now", "51000000\n"); /* 85% >= 80% */
    set_now(t0 + 90);
    assert(record_snapshot(0) == 1);
    assert(bat_read_line(bat_supply_root(), "BAT0", "charge_behaviour", line, sizeof line) == 0 &&
           strcmp(line, "auto [inhibit-charge]") == 0);
    assert(rel_exists(old_bl) && rel_exists(new_bl));

    /* Old pack returns at 70% (below start): its own baseline restores auto and is removed;
     * SANYO's baseline remains untouched. */
    put_txt("sys/BAT0/manufacturer", "Panasonic\n");
    put_txt("sys/BAT0/model_name", "CF-VZSU1QJS\n");
    put_txt("sys/BAT0/serial_number", "9001\n");
    put_txt("sys/BAT0/status", "Not charging\n");
    put_txt("sys/BAT0/energy_now", "38500000\n"); /* 70% <= 75% */
    put_txt("sys/BAT0/energy_full", "55000000\n");
    put_txt("sys/BAT0/energy_full_design", "55000000\n");
    put_txt(behav, "auto [inhibit-charge]\n");
    set_now(t0 + 120);
    assert(record_snapshot(0) == 1);
    charge_status_for("BAT0", "manual", 80, 80, 0);
    assert(bat_read_line(bat_supply_root(), "BAT0", "charge_behaviour", line, sizeof line) == 0 &&
           strcmp(line, "[auto] inhibit-charge") == 0);
    assert(!rel_exists(old_bl) && rel_exists(new_bl));
}

int main(int argc, char **argv)
{
    assert(argc == 2 && argv[1][0] == '/');
    snprintf(root, sizeof root, "%s", argv[1]);
    mkd("sys");
    mkd("hist");

    char p_sys[1280], p_hist[1280], p_rc[1280];
    snprintf(p_sys, sizeof p_sys, "%s/sys", root);
    snprintf(p_hist, sizeof p_hist, "%s/hist", root);
    snprintf(p_rc, sizeof p_rc, "%s/rc.conf", root);
    assert(setenv("LOCACHY_POWER_SUPPLY_DIR", p_sys, 1) == 0);
    assert(setenv("LOCACHY_BATTERY_HISTORY_DIR", p_hist, 1) == 0);
    assert(setenv("LOCACHY_RC_CONF", p_rc, 1) == 0);
    char p_bl[1280];
    snprintf(p_bl, sizeof p_bl, "%s/backlight", root);
    mkd("backlight");
    assert(setenv("LOCACHY_BACKLIGHT_DIR", p_bl, 1) == 0);

    put_txt("rc.conf", "battery_charge_start=\"75\"\nbattery_charge_end=\"80\"\n");

    int64_t t0 = 1762000000LL;

    /* Step 1: Pack A (LGC 45N1025 #1111) discharging in BAT0 at 82% */
    mkd("sys/AC");
    put_txt("sys/AC/type", "Mains\n");
    put_txt("sys/AC/online", "0\n");
    mkd("sys/BAT0");
    put_txt("sys/BAT0/type", "Battery\n");
    put_txt("sys/BAT0/present", "1\n");
    put_txt("sys/BAT0/status", "Discharging\n");
    put_txt("sys/BAT0/manufacturer", "LGC\n");
    put_txt("sys/BAT0/model_name", "45N1025\n");
    put_txt("sys/BAT0/serial_number", "1111\n");
    put_txt("sys/BAT0/energy_now", "41000000\n");
    put_txt("sys/BAT0/energy_full", "50000000\n");
    put_txt("sys/BAT0/energy_full_design", "56000000\n");
    put_txt("sys/BAT0/power_now", "9500000\n");
    put_txt("sys/BAT0/voltage_now", "11400000\n");
    put_txt("sys/BAT0/charge_control_start_threshold", "40\n");
    put_txt("sys/BAT0/charge_control_end_threshold", "50\n");

    set_now(t0);
    load_previous_slots();
    assert(record_snapshot(1) == 1);
    /* Initial insertion raised thresholds 40..50 -> 75..80 in EC-safe order */
    assert(get_int("sys/BAT0/charge_control_start_threshold") == 75);
    assert(get_int("sys/BAT0/charge_control_end_threshold") == 80);

    /* Step 2: Plug in AC and hot-unplug Pack A (present=0) */
    put_txt("sys/AC/online", "1\n");
    put_txt("sys/BAT0/present", "0\n");
    set_now(t0 + 600);
    assert(record_snapshot(0) == 0);

    struct bat_sample samps[16];
    int ns = bat_load_history("LGC_45N1025_1111", NULL, t0 - 60, samps, 16);
    assert(ns == 2 && samps[0].state == 'D' && samps[1].state == 'R' && samps[1].ts == t0 + 600);

    /* Step 3: Hot-insert Pack B (SANYO 45N1029 #2222) into BAT0 with default 0..100 thresholds */
    put_txt("sys/BAT0/present", "1\n");
    put_txt("sys/BAT0/status", "Not charging\n");
    put_txt("sys/BAT0/manufacturer", "SANYO\n");
    put_txt("sys/BAT0/model_name", "45N1029\n");
    put_txt("sys/BAT0/serial_number", "2222\n");
    put_txt("sys/BAT0/energy_now", "58800000\n");
    put_txt("sys/BAT0/energy_full", "60000000\n");
    put_txt("sys/BAT0/energy_full_design", "63000000\n");
    put_txt("sys/BAT0/charge_control_start_threshold", "0\n");
    put_txt("sys/BAT0/charge_control_end_threshold", "100\n");
    set_now(t0 + 660);
    assert(record_snapshot(0) == 1);
    assert(get_int("sys/BAT0/charge_control_start_threshold") == 75);
    assert(get_int("sys/BAT0/charge_control_end_threshold") == 80);

    /* Step 4: Simulate hibernate + cold-swap to Pack C (Panasonic CF-VZSU1QJS #9001) + fresh daemon start */
    reset_daemon_memory();
    put_txt("sys/AC/online", "0\n");
    put_txt("sys/BAT0/status", "Discharging\n");
    put_txt("sys/BAT0/manufacturer", "Panasonic\n");
    put_txt("sys/BAT0/model_name", "CF-VZSU1QJS\n");
    put_txt("sys/BAT0/serial_number", "9001\n");
    put_txt("sys/BAT0/energy_now", "52000000\n");
    put_txt("sys/BAT0/energy_full", "55000000\n");
    put_txt("sys/BAT0/energy_full_design", "55000000\n");
    put_txt("sys/BAT0/charge_control_start_threshold", "0\n");
    put_txt("sys/BAT0/charge_control_end_threshold", "100\n");

    set_now(t0 + 7200);
    load_previous_slots();
    assert(record_snapshot(1) == 1);
    assert(get_int("sys/BAT0/charge_control_start_threshold") == 75);
    assert(get_int("sys/BAT0/charge_control_end_threshold") == 80);

    /* Outgoing Pack B must have received an 'R' marker on cold-swap detection */
    ns = bat_load_history("SANYO_45N1029_2222", NULL, t0 - 60, samps, 16);
    assert(ns == 2 && samps[0].state == 'N' && samps[1].state == 'R');

    /* bat_scan must return live Pack C first, plus both offline spare packs A and B! */
    struct bat_device devs[8];
    struct bat_ac_info ac;
    int nd = bat_scan(devs, 8, &ac);
    assert(nd == 3 && !devs[0].is_offline_pack && !strcmp(devs[0].pack_id, "Panasonic_CF-VZSU1QJS_9001"));
    assert(devs[1].is_offline_pack && devs[2].is_offline_pack);

    /* Step 5: Dual-battery PowerBridge active discharge handoff (BAT1 -> BAT0) */
    mkd("sys/BAT1");
    put_txt("sys/BAT1/type", "Battery\n");
    put_txt("sys/BAT1/present", "1\n");
    put_txt("sys/BAT1/status", "Discharging\n");
    put_txt("sys/BAT1/manufacturer", "LGC\n");
    put_txt("sys/BAT1/model_name", "BridgeExt\n");
    put_txt("sys/BAT1/serial_number", "7777\n");
    put_txt("sys/BAT1/energy_now", "4000000\n");
    put_txt("sys/BAT1/energy_full", "24000000\n");
    put_txt("sys/BAT1/energy_full_design", "24000000\n");
    put_txt("sys/BAT0/status", "Not charging\n"); /* BAT0 waiting while BAT1 drains */

    set_now(t0 + 7300);
    assert(record_snapshot(0) == 2);
    assert(!strcmp(active_dis_slot, "BAT1"));

    /* BAT1 hits 5% and idles; BAT0 seamlessly takes over discharge */
    put_txt("sys/BAT1/energy_now", "1200000\n");
    put_txt("sys/BAT1/status", "Not charging\n");
    put_txt("sys/BAT0/status", "Discharging\n");
    set_now(t0 + 7400);
    assert(record_snapshot(0) == 2);
    assert(!strcmp(active_dis_slot, "BAT0"));

    struct bat_handoff_event evs[16];
    int nev = bat_load_handoffs(t0, evs, 16);
    /* Expected events: REMOVE (Pack A), INSERT (Pack B), SWAP (Pack B -> Pack C),
     * INSERT (BAT1 BridgeExt), DISCHARGE (BAT0->BAT1 on bridge attach),
     * DISCHARGE (BAT1->BAT0 when bridge drains to 5%). */
    assert(nev == 6);
    assert(!strcmp(evs[0].kind, "REMOVE") && !strcmp(evs[0].pack_a, "LGC_45N1025_1111"));
    assert(!strcmp(evs[1].kind, "INSERT") && !strcmp(evs[1].pack_b, "SANYO_45N1029_2222"));
    assert(!strcmp(evs[2].kind, "SWAP") && !strcmp(evs[2].pack_a, "SANYO_45N1029_2222") &&
           !strcmp(evs[2].pack_b, "Panasonic_CF-VZSU1QJS_9001"));
    assert(!strcmp(evs[3].kind, "INSERT") && !strcmp(evs[3].slot, "BAT1"));
    assert(!strcmp(evs[4].kind, "DISCHARGE") && !strcmp(evs[4].slot, "BAT0->BAT1"));
    assert(!strcmp(evs[5].kind, "DISCHARGE") && !strcmp(evs[5].slot, "BAT1->BAT0"));

    puts("PASS removable-battery handoff: per-pack identity, AC hot-unplug/insert, hibernate cold-swap, "
         "dual-battery BAT1->BAT0 discharge handoff, and automatic EC threshold re-apply");
    test_adaptive(t0 + 10000);
    puts("PASS adaptive battery charge handoff: model gate, unsupported/no-write, readback, per-pack restore, "
         "one-time full expiry, restart persistence, and status");
    test_sw_single_pack_swap(t0 + 20000);
    puts("PASS software charge_behaviour single-pack swap: per-pack baseline isolation and hysteresis handoff");
    return 0;
}
