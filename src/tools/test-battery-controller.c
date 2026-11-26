/* Simulated-clock tests for battery-controller.{h,c}
 * (pure-C port of ChromiumOS powerd ChargeHistory & AdaptiveChargingController).
 *
 * Each test prints "PASS <desc>" or "FAIL <desc>". Exits 0 only if all pass. */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <limits.h>
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../progs/battery-controller.h"

/* Arbitrary UTC midnight anchor: 2026-01-31 00:00:00 UTC (Saturday).
 * 2026-01-31T00:00:00Z = 1769817600 (divisible by 86400: 1769817600 / 86400 = 20484). */
#define BASE_DAY_UTC  1769817600LL
#define H(hours)      ((int64_t)(hours) * 3600LL)
#define M(mins)       ((int64_t)(mins) * 60LL)
#define D(days)       ((int64_t)(days) * BAT_CTRL_DAY_SEC)

static int g_pass = 0;
static int g_fail = 0;

#define CHECK(cond) do { \
    if (!(cond)) { \
        fprintf(stderr, "  check failed at line %d: %s\n", __LINE__, #cond); \
        return false; \
    } \
} while (0)

static bool near(double a, double b)
{
    if (isinf(a) && isinf(b))
        return (a > 0) == (b > 0);
    return fabs(a - b) <= 1e-6;
}

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

/* Build a controller populated with `ndays` of history ending at `today_utc`,
 * each day having `on_ac_h` hours on AC, `full_h` hours full, `hold_h` hours hold,
 * plus `nevents` completed events of durations 1h, 2h, ..., nevents h.
 * Starts on battery at wall = today_utc, boot = boot_sec. */
static bool setup_controller_with_history(struct bat_ctrl *c,
                                          int ndays,
                                          int on_ac_h,
                                          int full_h,
                                          int hold_h,
                                          int nevents,
                                          int64_t today_utc,
                                          double boot_sec)
{
    if (bat_ctrl_init(c, NULL) != 0)
        return false;

    struct bat_history_snapshot snap;
    memset(&snap, 0, sizeof(snap));

    if (ndays > BAT_CTRL_MAX_RETENTION_DAYS)
        ndays = BAT_CTRL_MAX_RETENTION_DAYS;
    for (int i = 0; i < ndays; i++) {
        int offset = ndays - 1 - i;
        snap.days[snap.day_count++] = (struct bat_daily_summary){
            .day_utc_sec = today_utc - D(offset),
            .time_on_ac_sec = H(on_ac_h),
            .time_full_on_ac_sec = H(full_h),
            .hold_time_on_ac_sec = H(hold_h),
        };
    }

    if (nevents > BAT_CTRL_MAX_CHARGE_EVENTS)
        nevents = BAT_CTRL_MAX_CHARGE_EVENTS;
    for (int i = 0; i < nevents; i++) {
        /* Place oldest first so start_wall_sec is strictly ascending. */
        int idx_from_newest = nevents - i; /* nevents .. 1 */
        snap.events[snap.event_count++] = (struct bat_charge_event){
            .start_wall_sec = today_utc - (int64_t)idx_from_newest * H(12),
            .duration_sec = H(i + 1),
        };
    }

    struct bat_ctrl_power off_ac = {
        .ac_connected = false,
        .battery_full = false,
        .battery_pct = 50.0,
        .display_battery_pct = 50.0,
    };
    return bat_ctrl_import_history(c, &snap, &off_ac, today_utc, boot_sec) == 0;
}

/* ---- 1. Learning gate: < 14 days or ratio < 0.5 -> HEURISTIC_DISABLED ---- */

static bool test_learning_gate_days(void)
{
    struct bat_ctrl c;
    /* 13 days with high ratio (4/5 = 0.8 >= 0.5): still blocked on days. */
    CHECK(setup_controller_with_history(&c, 13, 5, 3, 1, 10, BASE_DAY_UTC, 100.0));
    CHECK(bat_hist_days_of_history(&c.history) == 13);

    struct bat_ctrl_power plug = { true, false, 60.0, 60.0 };
    (void)bat_ctrl_on_power(&c, &plug, BASE_DAY_UTC, 100.0);
    CHECK(c.state == BAT_CTRL_STATE_HEURISTIC_DISABLED);
    CHECK(c.prediction_requested);

    double probs[BAT_ADAPTIVE_OUTPUTS] = { 0, 0, 0, 0, 0, 0, 0, 1.0, 0 };
    struct bat_ctrl_decision d = bat_ctrl_on_prediction(&c, true, probs, BASE_DAY_UTC, 100.0);
    CHECK(d.action == BAT_CTRL_ACTION_NONE);
    CHECK(d.sustain_percent == -1);
    CHECK(!c.is_sustain_set);

    /* Now with 14 days and ratio 0.6 -> ACTIVE and HOLD. */
    CHECK(setup_controller_with_history(&c, 14, 5, 2, 1, 10, BASE_DAY_UTC, 100.0));
    (void)bat_ctrl_on_power(&c, &plug, BASE_DAY_UTC, 100.0);
    CHECK(c.state == BAT_CTRL_STATE_ACTIVE);
    d = bat_ctrl_on_prediction(&c, true, probs, BASE_DAY_UTC, 100.0);
    CHECK(d.action == BAT_CTRL_ACTION_HOLD);
    CHECK(d.sustain_percent == 80);
    CHECK(c.is_sustain_set);
    return true;
}

static bool test_learning_gate_ratio(void)
{
    struct bat_ctrl c;
    /* 14 days, 5 h on AC, 1 h full + 1 h hold -> ratio 2/5 = 0.4 < 0.5. */
    CHECK(setup_controller_with_history(&c, 14, 5, 1, 1, 10, BASE_DAY_UTC, 100.0));
    struct bat_ctrl_power plug = { true, false, 50.0, 50.0 };
    (void)bat_ctrl_on_power(&c, &plug, BASE_DAY_UTC, 100.0);
    CHECK(c.state == BAT_CTRL_STATE_HEURISTIC_DISABLED);

    double probs[BAT_ADAPTIVE_OUTPUTS] = { 0, 0, 0, 0, 0, 0, 0, 0, 1.0 };
    struct bat_ctrl_decision d = bat_ctrl_on_prediction(&c, true, probs, BASE_DAY_UTC, 100.0);
    CHECK(d.action == BAT_CTRL_ACTION_NONE);
    CHECK(!c.is_sustain_set);

    /* Zero on_ac time across 14 days -> ratio 0.0 -> HEURISTIC_DISABLED. */
    CHECK(setup_controller_with_history(&c, 14, 0, 0, 0, 0, BASE_DAY_UTC, 100.0));
    (void)bat_ctrl_on_power(&c, &plug, BASE_DAY_UTC, 100.0);
    CHECK(c.state == BAT_CTRL_STATE_HEURISTIC_DISABLED);
    return true;
}

/* ---- 2. 9-bucket CDF search ---- */

static bool test_cdf_buckets(void)
{
    struct bat_ctrl c;
    CHECK(setup_controller_with_history(&c, 15, 5, 2, 1, 15, BASE_DAY_UTC, 1000.0));
    struct bat_ctrl_power plug = { true, false, 60.0, 60.0 };
    (void)bat_ctrl_on_power(&c, &plug, BASE_DAY_UTC, 1000.0);
    CHECK(c.state == BAT_CTRL_STATE_ACTIVE);

    /* Bucket 0 == 0.35 (>= min_probability 0.35) -> target_delay = 0 h -> RESUME. */
    double p0[BAT_ADAPTIVE_OUTPUTS] = { 0.35, 0.05, 0.1, 0.1, 0.1, 0.1, 0.1, 0.05, 0.05 };
    struct bat_ctrl_decision d = bat_ctrl_on_prediction(&c, true, p0, BASE_DAY_UTC, 1000.0);
    CHECK(d.action == BAT_CTRL_ACTION_RESUME);
    CHECK(c.state == BAT_CTRL_STATE_INACTIVE);
    CHECK(near(d.target_full_at, 1000.0)); /* not holding -> now + 0 h */
    CHECK(near(d.release_at, -1.0));
    CHECK(!bat_ctrl_is_running(&c));

    /* Re-plug and check cumulative sum crossing at bucket 4 (4 h -> release at +2 h). */
    struct bat_ctrl_power unplug = { false, false, 60.0, 60.0 };
    (void)bat_ctrl_on_power(&c, &unplug, BASE_DAY_UTC + 60, 1060.0);
    (void)bat_ctrl_on_power(&c, &plug, BASE_DAY_UTC + 120, 1120.0);
    CHECK(c.state == BAT_CTRL_STATE_ACTIVE);

    /* Cumulative: 0.08, 0.16, 0.24, 0.32, 0.40 (>= 0.35 at hour 4). */
    double p4[BAT_ADAPTIVE_OUTPUTS] = { 0.08, 0.08, 0.08, 0.08, 0.08, 0.1, 0.1, 0.2, 0.2 };
    d = bat_ctrl_on_prediction(&c, true, p4, BASE_DAY_UTC + 120, 1120.0);
    CHECK(d.action == BAT_CTRL_ACTION_HOLD);
    CHECK(d.sustain_percent == 80);
    CHECK(near(d.release_at, 1120.0 + 2.0 * 3600.0));
    CHECK(near(d.target_full_at, 1120.0 + 4.0 * 3600.0));
    CHECK(near(d.recheck_at, 1120.0 + 1800.0));

    /* Mass only in bucket 8 -> unbounded target, HOLD, release_at == -1, target == +inf. */
    (void)bat_ctrl_on_power(&c, &unplug, BASE_DAY_UTC + 200, 1200.0);
    (void)bat_ctrl_on_power(&c, &plug, BASE_DAY_UTC + 300, 1300.0);
    double p8[BAT_ADAPTIVE_OUTPUTS] = { 0, 0, 0, 0, 0, 0, 0, 0, 1.0 };
    d = bat_ctrl_on_prediction(&c, true, p8, BASE_DAY_UTC + 300, 1300.0);
    CHECK(d.action == BAT_CTRL_ACTION_HOLD);
    CHECK(d.sustain_percent == 80);
    CHECK(near(d.release_at, -1.0));
    CHECK(isinf(d.target_full_at) && d.target_full_at > 0);
    CHECK(near(d.recheck_at, 1300.0 + 1800.0));

    /* Sustain rule: battery at 87% > hold_percent (80) -> sustain = 87. */
    (void)bat_ctrl_on_power(&c, &unplug, BASE_DAY_UTC + 400, 1400.0);
    struct bat_ctrl_power plug87 = { true, false, 87.0, 87.4 };
    (void)bat_ctrl_on_power(&c, &plug87, BASE_DAY_UTC + 500, 1500.0);
    d = bat_ctrl_on_prediction(&c, true, p8, BASE_DAY_UTC + 500, 1500.0);
    CHECK(d.action == BAT_CTRL_ACTION_HOLD);
    CHECK(d.sustain_percent == 87);

    /* When already holding and prediction drops to <= 2 h -> RESUME, deadline = now + 2 h. */
    double p1[BAT_ADAPTIVE_OUTPUTS] = { 0.0, 0.5, 0.5, 0, 0, 0, 0, 0, 0 };
    d = bat_ctrl_on_prediction(&c, true, p1, BASE_DAY_UTC + 600, 1600.0);
    CHECK(d.action == BAT_CTRL_ACTION_RESUME);
    CHECK(near(d.target_full_at, 1600.0 + 2.0 * 3600.0));
    CHECK(c.state == BAT_CTRL_STATE_INACTIVE);
    return true;
}

/* ---- 3. Percentile cap and promised-deadline preservation ---- */

static bool test_percentile_cap_and_no_pushout(void)
{
    struct bat_ctrl c;
    /* 15 events with durations 1h..15h; 30th percentile = index ceil(0.3*15)=ceil(4.5)=5 -> 6h. */
    CHECK(setup_controller_with_history(&c, 15, 5, 2, 1, 15, BASE_DAY_UTC, 100.0));
    CHECK(bat_hist_duration_percentile_sec(&c.history, 0.3) == H(6));

    /* Plug in at 80% so hold_start is recorded immediately upon reaching hold. */
    struct bat_ctrl_power plug80 = { true, false, 80.0, 80.0 };
    (void)bat_ctrl_on_power(&c, &plug80, BASE_DAY_UTC, 100.0);

    /* First prediction (bucket 8 = unbounded) while hold_start is not yet set. */
    double p8[BAT_ADAPTIVE_OUTPUTS] = { 0, 0, 0, 0, 0, 0, 0, 0, 1.0 };
    struct bat_ctrl_decision d = bat_ctrl_on_prediction(&c, true, p8, BASE_DAY_UTC, 100.0);
    CHECK(d.action == BAT_CTRL_ACTION_HOLD);
    CHECK(c.hold_start_valid && near(c.hold_start_boot_sec, 100.0));

    /* Next recheck at t=1900 (30 min later): bucket 8 is clamped by 6h cap from
     * hold_start (100 + 6h = 21700), so release_at = 21700 - 2h = 14500. */
    (void)bat_ctrl_tick(&c, BASE_DAY_UTC + 1800, 1900.0);
    CHECK(c.prediction_requested);
    CHECK(c.prediction_reason == BAT_CTRL_REASON_PERIODIC_LOG);
    d = bat_ctrl_on_prediction(&c, true, p8, BASE_DAY_UTC + 1800, 1900.0);
    CHECK(d.action == BAT_CTRL_ACTION_HOLD);
    CHECK(near(d.target_full_at, 100.0 + 6.0 * 3600.0));
    CHECK(near(d.release_at, 100.0 + 4.0 * 3600.0));

    /* Prediction shifts earlier (hour 3 from 1900 -> target 1900 + 3h = 12700 < 21700):
     * allowed to move earlier. */
    double p3[BAT_ADAPTIVE_OUTPUTS] = { 0, 0, 0, 1.0, 0, 0, 0, 0, 0 };
    d = bat_ctrl_on_prediction(&c, true, p3, BASE_DAY_UTC + 1800, 1900.0);
    CHECK(d.action == BAT_CTRL_ACTION_HOLD);
    CHECK(near(d.target_full_at, 1900.0 + 3.0 * 3600.0));
    CHECK(near(d.release_at, 1900.0 + 1.0 * 3600.0));

    /* Prediction tries to push later (hour 5 -> 1900 + 5h > 12700) while at hold
     * and charge_alarm is running: upstream keeps the earlier promised deadline. */
    double p5[BAT_ADAPTIVE_OUTPUTS] = { 0, 0, 0, 0, 0, 1.0, 0, 0, 0 };
    d = bat_ctrl_on_prediction(&c, true, p5, BASE_DAY_UTC + 1800, 1900.0);
    CHECK(d.action == BAT_CTRL_ACTION_HOLD);
    CHECK(near(d.target_full_at, 1900.0 + 3.0 * 3600.0));
    CHECK(near(d.release_at, 1900.0 + 1.0 * 3600.0));

    /* When charge_alarm expires, tick emits RESUME and transitions to INACTIVE. */
    d = bat_ctrl_tick(&c, BASE_DAY_UTC + 1800 + 3600, 1900.0 + 3600.0);
    CHECK(d.action == BAT_CTRL_ACTION_RESUME);
    CHECK(c.state == BAT_CTRL_STATE_INACTIVE);
    CHECK(!c.is_sustain_set);

    /* Short percentile history (2h): target_delay <= 2h -> immediate RESUME. */
    struct bat_ctrl c2;
    CHECK(bat_ctrl_init(&c2, NULL) == 0);
    struct bat_history_snapshot snap;
    memset(&snap, 0, sizeof(snap));
    for (int i = 0; i < 14; i++) {
        snap.days[snap.day_count++] = (struct bat_daily_summary){
            .day_utc_sec = BASE_DAY_UTC - D(13 - i),
            .time_on_ac_sec = H(4),
            .time_full_on_ac_sec = H(3),
            .hold_time_on_ac_sec = 0,
        };
    }
    for (int i = 0; i < 10; i++) {
        snap.events[snap.event_count++] = (struct bat_charge_event){
            .start_wall_sec = BASE_DAY_UTC - (int64_t)(10 - i) * H(6),
            .duration_sec = H(2),
        };
    }
    struct bat_ctrl_power off_ac = { false, false, 80.0, 80.0 };
    CHECK(bat_ctrl_import_history(&c2, &snap, &off_ac, BASE_DAY_UTC, 50.0) == 0);
    (void)bat_ctrl_on_power(&c2, &plug80, BASE_DAY_UTC, 50.0);
    d = bat_ctrl_on_prediction(&c2, true, p8, BASE_DAY_UTC, 50.0);
    CHECK(d.action == BAT_CTRL_ACTION_HOLD); /* hold_start recorded at 50.0 */
    d = bat_ctrl_on_prediction(&c2, true, p8, BASE_DAY_UTC + 10, 60.0); /* 50 + 2h - 60 = 7190 <= 7200 */
    CHECK(d.action == BAT_CTRL_ACTION_RESUME);
    CHECK(near(d.target_full_at, 60.0 + 2.0 * 3600.0));
    return true;
}

/* ---- 4. 96% guard and FULL downgrade ---- */

static bool test_96_guard_and_full_downgrade(void)
{
    struct bat_ctrl c;
    CHECK(setup_controller_with_history(&c, 15, 5, 2, 1, 15, BASE_DAY_UTC, 100.0));

    /* Plug in at display_battery_pct == 96.0 -> downgraded to INACTIVE, no prediction. */
    struct bat_ctrl_power plug96 = { true, false, 95.0, 96.0 };
    struct bat_ctrl_decision d = bat_ctrl_on_power(&c, &plug96, BASE_DAY_UTC, 100.0);
    CHECK(c.state == BAT_CTRL_STATE_INACTIVE);
    CHECK(!c.prediction_requested);
    CHECK(!c.started);
    CHECK(d.action == BAT_CTRL_ACTION_NONE);

    /* Unplug, then plug in with battery_full == true at 92% -> INACTIVE. */
    struct bat_ctrl_power unplug = { false, false, 92.0, 92.0 };
    (void)bat_ctrl_on_power(&c, &unplug, BASE_DAY_UTC + 60, 160.0);
    struct bat_ctrl_power plug_full = { true, true, 92.0, 92.0 };
    (void)bat_ctrl_on_power(&c, &plug_full, BASE_DAY_UTC + 120, 220.0);
    CHECK(c.state == BAT_CTRL_STATE_INACTIVE);
    CHECK(!c.prediction_requested);

    /* Unplug, plug in at 95.9% (below 96%) -> ACTIVE & HOLD, then FULL downgrades it. */
    (void)bat_ctrl_on_power(&c, &unplug, BASE_DAY_UTC + 180, 280.0);
    struct bat_ctrl_power plug95 = { true, false, 95.0, 95.9 };
    (void)bat_ctrl_on_power(&c, &plug95, BASE_DAY_UTC + 240, 340.0);
    CHECK(c.state == BAT_CTRL_STATE_ACTIVE);
    CHECK(c.prediction_requested);
    double p7[BAT_ADAPTIVE_OUTPUTS] = { 0, 0, 0, 0, 0, 0, 0, 1.0, 0 };
    d = bat_ctrl_on_prediction(&c, true, p7, BASE_DAY_UTC + 240, 340.0);
    CHECK(d.action == BAT_CTRL_ACTION_HOLD);
    CHECK(d.sustain_percent == 95);

    struct bat_ctrl_power now_full = { true, true, 100.0, 100.0 };
    d = bat_ctrl_on_power(&c, &now_full, BASE_DAY_UTC + 600, 700.0);
    CHECK(c.state == BAT_CTRL_STATE_INACTIVE);
    CHECK(d.action == BAT_CTRL_ACTION_RESUME);
    CHECK(!c.is_sustain_set);
    return true;
}

/* ---- 5. Charge to Full Now (USER_CANCELED) ---- */

static bool test_charge_now(void)
{
    struct bat_ctrl c;
    CHECK(setup_controller_with_history(&c, 15, 5, 2, 1, 15, BASE_DAY_UTC, 100.0));
    struct bat_ctrl_power plug = { true, false, 70.0, 70.0 };
    (void)bat_ctrl_on_power(&c, &plug, BASE_DAY_UTC, 100.0);
    double p7[BAT_ADAPTIVE_OUTPUTS] = { 0, 0, 0, 0, 0, 0, 0, 1.0, 0 };
    struct bat_ctrl_decision d = bat_ctrl_on_prediction(&c, true, p7, BASE_DAY_UTC, 100.0);
    CHECK(d.action == BAT_CTRL_ACTION_HOLD);
    CHECK(c.state == BAT_CTRL_STATE_ACTIVE);

    /* Charge Now -> USER_CANCELED, RESUME, alarms cleared. */
    d = bat_ctrl_charge_now(&c, BASE_DAY_UTC + 300, 400.0);
    CHECK(c.state == BAT_CTRL_STATE_USER_CANCELED);
    CHECK(d.action == BAT_CTRL_ACTION_RESUME);
    CHECK(!c.is_sustain_set);
    CHECK(!bat_ctrl_is_running(&c));
    CHECK(!c.charge_alarm_running);

    /* Subsequent power updates and ticks while still plugged stay USER_CANCELED. */
    struct bat_ctrl_power climb = { true, false, 85.0, 85.0 };
    d = bat_ctrl_on_power(&c, &climb, BASE_DAY_UTC + 1800, 1900.0);
    CHECK(c.state == BAT_CTRL_STATE_USER_CANCELED);
    CHECK(d.action == BAT_CTRL_ACTION_NONE);
    CHECK(!c.prediction_requested);

    d = bat_ctrl_tick(&c, BASE_DAY_UTC + 3600, 3700.0);
    CHECK(c.state == BAT_CTRL_STATE_USER_CANCELED);
    CHECK(d.action == BAT_CTRL_ACTION_NONE);
    CHECK(!c.prediction_requested);

    /* Next unplug + replug re-activates adaptive charging. */
    struct bat_ctrl_power unplug = { false, false, 85.0, 85.0 };
    (void)bat_ctrl_on_power(&c, &unplug, BASE_DAY_UTC + 4000, 4100.0);
    (void)bat_ctrl_on_power(&c, &plug, BASE_DAY_UTC + 4500, 4600.0);
    CHECK(c.state == BAT_CTRL_STATE_ACTIVE);
    CHECK(c.prediction_requested);
    d = bat_ctrl_on_prediction(&c, true, p7, BASE_DAY_UTC + 4500, 4600.0);
    CHECK(d.action == BAT_CTRL_ACTION_HOLD);
    return true;
}

/* ---- 6. Shutdown clears state and blocks further activation ---- */

static bool test_shutdown(void)
{
    struct bat_ctrl c;
    CHECK(setup_controller_with_history(&c, 15, 5, 2, 1, 15, BASE_DAY_UTC, 100.0));
    struct bat_ctrl_power plug = { true, false, 70.0, 70.0 };
    (void)bat_ctrl_on_power(&c, &plug, BASE_DAY_UTC, 100.0);
    double p7[BAT_ADAPTIVE_OUTPUTS] = { 0, 0, 0, 0, 0, 0, 0, 1.0, 0 };
    struct bat_ctrl_decision d = bat_ctrl_on_prediction(&c, true, p7, BASE_DAY_UTC, 100.0);
    CHECK(d.action == BAT_CTRL_ACTION_HOLD);

    d = bat_ctrl_on_shutdown(&c, BASE_DAY_UTC + 600, 700.0);
    CHECK(c.state == BAT_CTRL_STATE_SHUTDOWN);
    CHECK(d.action == BAT_CTRL_ACTION_RESUME);
    CHECK(!c.is_sustain_set);
    CHECK(!bat_ctrl_is_running(&c));
    CHECK(!c.charge_alarm_running);
    CHECK(!c.cfg.enabled);

    /* Re-enabling via config or unplug/replug must stay SHUTDOWN. */
    struct bat_ctrl_config cfg;
    bat_ctrl_default_config(&cfg);
    (void)bat_ctrl_set_config(&c, &cfg, BASE_DAY_UTC + 650, 750.0);
    CHECK(c.state == BAT_CTRL_STATE_SHUTDOWN);

    struct bat_ctrl_power unplug = { false, false, 70.0, 70.0 };
    (void)bat_ctrl_on_power(&c, &unplug, BASE_DAY_UTC + 700, 800.0);
    (void)bat_ctrl_on_power(&c, &plug, BASE_DAY_UTC + 750, 850.0);
    CHECK(c.state == BAT_CTRL_STATE_SHUTDOWN);
    CHECK(!c.prediction_requested);
    return true;
}

/* ---- 7. Support / user-disable states & suspend/resume ---- */

static bool test_support_user_disabled_and_suspend(void)
{
    struct bat_ctrl_config cfg;
    bat_ctrl_default_config(&cfg);
    cfg.supported = false;
    cfg.enabled = true;
    struct bat_ctrl c;
    CHECK(bat_ctrl_init(&c, &cfg) == 0);
    CHECK(c.state == BAT_CTRL_STATE_NOT_SUPPORTED);
    CHECK(!c.cfg.enabled);

    bat_ctrl_default_config(&cfg);
    cfg.enabled = false;
    CHECK(bat_ctrl_init(&c, &cfg) == 0);
    CHECK(c.state == BAT_CTRL_STATE_USER_DISABLED);

    /* Suspend while running requests a SUSPEND prediction and flushes full/hold. */
    CHECK(setup_controller_with_history(&c, 15, 5, 2, 1, 15, BASE_DAY_UTC, 100.0));
    struct bat_ctrl_power plug80 = { true, false, 80.0, 80.0 };
    (void)bat_ctrl_on_power(&c, &plug80, BASE_DAY_UTC, 100.0);
    double p7[BAT_ADAPTIVE_OUTPUTS] = { 0, 0, 0, 0, 0, 0, 0, 1.0, 0 };
    (void)bat_ctrl_on_prediction(&c, true, p7, BASE_DAY_UTC, 100.0);
    /* Hold tracking started immediately in bat_ctrl_on_prediction. */
    CHECK(c.history.has_hold_charge);

    int64_t hold_before = c.history.hold_duration_on_ac_sec;
    (void)bat_ctrl_on_suspend(&c, BASE_DAY_UTC + H(1), 100.0 + 3600.0);
    CHECK(c.prediction_requested);
    CHECK(c.prediction_reason == BAT_CTRL_REASON_SUSPEND);
    CHECK(c.history.hold_duration_on_ac_sec == hold_before + H(1));

    bat_ctrl_on_resume(&c, BASE_DAY_UTC + H(2), 100.0 + 7200.0);
    return true;
}

/* ---- 8. ChargeHistory: 15-min flooring, 50-session cap, 30-day retention & zero-fill ---- */

static bool test_history_flooring_cap_retention(void)
{
    struct bat_charge_history h;
    bat_hist_init(&h);

    /* Plug in at 00:07, unplug at 00:50 (43 min elapsed):
     * start floored to 00:00, duration floored to 30 min (1800 s). */
    struct bat_ctrl_power on  = { true, false, 50.0, 50.0 };
    struct bat_ctrl_power off = { false, false, 60.0, 60.0 };
    bat_hist_update(&h, &on, false, BASE_DAY_UTC + M(7), 100.0);
    CHECK(h.has_ac_connect);
    CHECK(h.ac_connect_wall_sec == BASE_DAY_UTC);
    bat_hist_update(&h, &off, false, BASE_DAY_UTC + M(50), 100.0 + (double)M(43));
    CHECK(!h.has_ac_connect);
    CHECK(h.event_count == 1);
    CHECK(h.events[0].start_wall_sec == BASE_DAY_UTC);
    CHECK(h.events[0].duration_sec == M(30));
    CHECK(bat_hist_time_on_ac_sec(&h, 100.0 + (double)M(43)) == M(45));
    /* Wait: start_wall_sec = 00:00, unplug wall = 00:50 -> wall diff = 50 min ->
     * floored to 45 min in RecordDurations, while charge event uses boot ticks
     * (43 min -> 30 min). Verify exact upstream behavior! */

    /* 50-session cap (49 while active). Add 60 completed sessions spaced 1h apart. */
    bat_hist_init(&h);
    int64_t t = BASE_DAY_UTC;
    double b = 1000.0;
    for (int i = 0; i < 60; i++) {
        bat_hist_update(&h, &on, false, t, b);
        if (i >= 49)
            CHECK(h.event_count == 49); /* 49 completed while active */
        t += M(30);
        b += (double)M(30);
        bat_hist_update(&h, &off, false, t, b);
        t += M(30);
        b += (double)M(30);
    }
    CHECK(h.event_count == BAT_CTRL_MAX_CHARGE_EVENTS);
    /* Oldest retained should be session #10 (0-based), start = BASE_DAY_UTC + 10 h. */
    CHECK(h.events[0].start_wall_sec == BASE_DAY_UTC + H(10));
    CHECK(h.events[49].start_wall_sec == BASE_DAY_UTC + H(59));

    /* Plugging in drops completed count from 50 to 49. */
    bat_hist_update(&h, &on, false, t, b);
    CHECK(h.has_ac_connect);
    CHECK(h.event_count == 49);
    CHECK(h.events[0].start_wall_sec == BASE_DAY_UTC + H(11));

    /* 30-day daily retention and zero-fill across gaps. */
    bat_hist_init(&h);
    bat_hist_update(&h, &off, false, BASE_DAY_UTC, 10.0);
    CHECK(bat_hist_days_of_history(&h) == 1);

    /* Jump 5 days ahead -> zero-fills days 1..5 (total 6 days). */
    bat_hist_update(&h, &off, false, BASE_DAY_UTC + D(5), 10.0 + (double)D(5));
    CHECK(bat_hist_days_of_history(&h) == 6);
    for (size_t i = 0; i < h.day_count; i++) {
        CHECK(h.days[i].day_utc_sec == BASE_DAY_UTC + D((int64_t)i));
        CHECK(h.days[i].time_on_ac_sec == 0);
    }

    /* Jump to day 35 -> retention cutoff = day 5; days 0..4 pruned.
     * Days 5..35 (inclusive) at midnight: day 5 is NOT < (day 35 - 30d = day 5),
     * so at midnight + 1s (cutoff = day 5 + 1s), day 5 is strictly < cutoff -> 30 days. */
    bat_hist_update(&h, &off, false, BASE_DAY_UTC + D(35) + 1, 10.0 + (double)D(35) + 1.0);
    CHECK(bat_hist_days_of_history(&h) == 30);
    CHECK(h.days[0].day_utc_sec == BASE_DAY_UTC + D(6));
    CHECK(h.days[29].day_utc_sec == BASE_DAY_UTC + D(35));
    return true;
}

/* ---- 9. Percentile edge cases & clock-step rule ---- */

static bool test_percentile_and_clock_step(void)
{
    struct bat_charge_history h;
    bat_hist_init(&h);

    /* Empty history -> INT64_MAX for any valid percentile. */
    CHECK(bat_hist_duration_percentile_sec(&h, 0.0) == INT64_MAX);
    CHECK(bat_hist_duration_percentile_sec(&h, 0.3) == INT64_MAX);
    CHECK(bat_hist_duration_percentile_sec(&h, 1.0) == INT64_MAX);

    /* Add 15 events of 1h..15h in scrambled order via import. */
    struct bat_history_snapshot snap;
    memset(&snap, 0, sizeof(snap));
    int order[15] = { 7, 1, 15, 3, 12, 9, 4, 14, 2, 11, 6, 13, 5, 10, 8 };
    for (int i = 0; i < 15; i++) {
        snap.events[snap.event_count++] = (struct bat_charge_event){
            .start_wall_sec = BASE_DAY_UTC - (int64_t)(15 - i) * H(20),
            .duration_sec = H(order[i]),
        };
    }
    struct bat_ctrl_power off = { false, false, 50.0, 50.0 };
    CHECK(bat_hist_import(&h, &snap, &off, false, BASE_DAY_UTC, 100.0) == 0);

    /* Matches upstream TestGetChargeDurationPercentile: i/15 -> (i+1) hours. */
    for (int i = 0; i < 15; i++) {
        CHECK(bat_hist_duration_percentile_sec(&h, (double)i / 15.0) == H(i + 1));
    }
    CHECK(bat_hist_duration_percentile_sec(&h, 1.0) == INT64_MAX);

    /* Pure clock-step rule:
     * wall_elapsed = 30 min, boot_elapsed = 20 min -> floored 30 vs 15, diff = 15 <= 15 -> no step. */
    int64_t corr = -1;
    CHECK(!bat_hist_check_clock_step(BASE_DAY_UTC + M(30), M(30), (double)M(20), &corr));

    /* wall_elapsed = 3 days, boot_elapsed = 15 min -> diff > 15 min -> step!
     * corrected_start = floor(now) - floor(boot_elapsed). */
    CHECK(bat_hist_check_clock_step(BASE_DAY_UTC + D(3) + M(5), D(3) + M(5),
                                    (double)M(20), &corr));
    CHECK(corr == BASE_DAY_UTC + D(3) - M(15));

    /* End-to-end ChargeHistory clock jump (upstream TestSystemTimeJumpNoHistory):
     * plug in at old_wall, jump wall +3 days with 0 boot advance -> unplug yields 0 duration. */
    bat_hist_init(&h);
    int64_t old_wall = BASE_DAY_UTC - D(3);
    struct bat_ctrl_power on = { true, false, 60.0, 60.0 };
    bat_hist_update(&h, &on, false, old_wall, 500.0);
    CHECK(h.ac_connect_wall_sec == old_wall);

    /* Wall jumps +3 days while boot stays 500.0 -> disconnect records 0 duration at BASE_DAY_UTC. */
    bat_hist_update(&h, &off, false, BASE_DAY_UTC, 500.0);
    CHECK(h.event_count == 1);
    CHECK(h.events[0].start_wall_sec == BASE_DAY_UTC);
    CHECK(h.events[0].duration_sec == 0);
    CHECK(bat_hist_time_on_ac_sec(&h, 500.0) == 0);
    return true;
}

/* ---- 10. Feature vector: exact names, counts, kinds, -1 fill, golden check ---- */

static const struct bat_feature *find_feat(const struct bat_feature *arr, int n,
                                           const char *name)
{
    for (int i = 0; i < n; i++) {
        if (strcmp(arr[i].name, name) == 0)
            return &arr[i];
    }
    return NULL;
}

static bool test_feature_vector_golden(void)
{
    /* Handcrafted history anchored at today = BASE_DAY_UTC (2026-01-31 00:00Z).
     * Query time: wall_sec = BASE_DAY_UTC + 10h 37m (637 min past UTC midnight),
     * boot_sec = 5000.0.
     *
     * Completed charge events (2 total):
     *   Event A (older): start = BASE_DAY_UTC - 24h (day -1 00:00Z), dur = 4h (240 min)
     *   Event B (newer): start = BASE_DAY_UTC + 2h  (day  0 02:00Z), dur = 2h (120 min)
     * Active AC session (currently plugged in):
     *   Plugged in at wall = BASE_DAY_UTC + 8h 37m (floored to +8h 30m = 510 min),
     *   boot = 5000.0 - 2h (boot_sec - 7200 = -2200 -> use boot=10000 at query, plug at 2800).
     *   So active session has elapsed 2h (120 min) of boot time and 2h 07m wall time.
     *   Active start_wall_sec = BASE_DAY_UTC + 8h 30m.
     *   wall_sec - active_start = (10h 37m) - (8h 30m) = 2h 07m = 127 min (integer div by 60).
     *   wall_sec - Event B start = (10h 37m) - (2h 00m) = 8h 37m = 517 min.
     *   wall_sec - Event A start = (24h + 10h 37m) = 34h 37m = 2077 min.
     *
     * Daily summaries stored before active session:
     *   Day -2 (BASE_DAY_UTC - 2d): on_ac = 6h (360m), full = 3h (180m), hold = 1h (60m)
     *   Day -1 (BASE_DAY_UTC - 1d): on_ac = 4h (240m), full = 2h (120m), hold = 0h (0m)
     *   Day  0 (BASE_DAY_UTC     ): on_ac = 2h (120m), full = 0h (0m),   hold = 0h (0m)
     *   Plus active session on Day 0: start = +8h 30m, floor(now) = +10h 30m -> +2h (120m)
     *   projected onto Day 0 time_on_ac -> Day 0 total time_on_ac = 4h (240m).
     *
     * Totals for RatioTimeFullCharge (at boot = 10000.0, active boot elapsed = 2h):
     *   time_on_ac = (6 + 4 + 2) + 2 = 14h = 50400s
     *   time_full  = (3 + 2 + 0)     =  5h = 18000s
     *   hold_time  = (1 + 0 + 0)     =  1h =  3600s
     *   RatioTimeFullCharge = int32(10 * (18000 + 3600) / 50400)
     *                       = int32(216000 / 50400) = int32(4.2857...) = 4.
     *
     * Local time context:
     *   local_hour = 14, local_minute = 37 -> 14*60 + 37 = 877 -> floored to 15-min = 870.
     *   day_of_week = 6 (Sat), day_of_month = 31, mode = LAPTOP (2),
     *   screen_brightness_pct = 65, reason = PERIODIC_LOG (3),
     *   battery_pct = 73.8 -> int32(73), IsCharging = 1.
     */
    struct bat_ctrl c;
    CHECK(bat_ctrl_init(&c, NULL) == 0);

    struct bat_history_snapshot snap;
    memset(&snap, 0, sizeof(snap));
    snap.days[0] = (struct bat_daily_summary){ BASE_DAY_UTC - D(2), H(6), H(3), H(1) };
    snap.days[1] = (struct bat_daily_summary){ BASE_DAY_UTC - D(1), H(4), H(2), 0 };
    snap.days[2] = (struct bat_daily_summary){ BASE_DAY_UTC,        H(2), 0,    0 };
    snap.day_count = 3;

    snap.events[0] = (struct bat_charge_event){ BASE_DAY_UTC - H(24), H(4) };
    snap.events[1] = (struct bat_charge_event){ BASE_DAY_UTC + H(2),  H(2) };
    snap.event_count = 2;

    int64_t plug_wall = BASE_DAY_UTC + H(8) + M(37);
    double plug_boot = 2800.0;
    struct bat_ctrl_power plugged = { true, false, 73.8, 74.0 };
    CHECK(bat_ctrl_import_history(&c, &snap, &plugged, plug_wall, plug_boot) == 0);

    int64_t query_wall = BASE_DAY_UTC + H(10) + M(37);
    double query_boot = 10000.0; /* plug_boot + 7200s */

    struct bat_ctrl_context ctx = {
        .local_hour = 14,
        .local_minute = 37,
        .day_of_week = 6,
        .day_of_month = 31,
        .mode = BAT_CTRL_MODE_LAPTOP,
        .screen_brightness_pct = 65,
        .reason = BAT_CTRL_REASON_PERIODIC_LOG,
    };

    /* Buffer too small must return -1. */
    struct bat_feature feats[BAT_CTRL_FEATURE_COUNT];
    CHECK(bat_ctrl_features(&c, &ctx, query_wall, query_boot, feats, 10) == -1);

    int n = bat_ctrl_features(&c, &ctx, query_wall, query_boot, feats,
                              BAT_CTRL_FEATURE_COUNT);
    CHECK(n == BAT_CTRL_FEATURE_COUNT);
    CHECK(n == 201);

    /* Every upstream feature in UpdateAdaptiveCharging uses int32_value. */
    for (int i = 0; i < n; i++) {
        CHECK(feats[i].name != NULL);
        CHECK(feats[i].kind == BAT_FEAT_INT32);
    }

    /* Check exact scalar values. */
    const struct bat_feature *f;
    CHECK((f = find_feat(feats, n, "TimeOfTheDay")) && f->value == 870.0);
    CHECK((f = find_feat(feats, n, "DayOfWeek")) && f->value == 6.0);
    CHECK((f = find_feat(feats, n, "DayOfMonth")) && f->value == 31.0);
    CHECK((f = find_feat(feats, n, "DeviceMode")) && f->value == 2.0);
    CHECK((f = find_feat(feats, n, "BatteryPercentage")) && f->value == 73.0);
    CHECK((f = find_feat(feats, n, "IsCharging")) && f->value == 1.0);
    CHECK((f = find_feat(feats, n, "ScreenBrightnessPercent")) && f->value == 65.0);
    CHECK((f = find_feat(feats, n, "Reason")) && f->value == 3.0);
    CHECK((f = find_feat(feats, n, "RatioTimeFullCharge")) && f->value == 4.0);

    /* ChargeEventHistorySize = 2 completed + 1 active = 3. */
    CHECK((f = find_feat(feats, n, "ChargeEventHistorySize")) && f->value == 3.0);

    /* Reverse-chronological event slots:
     *   slot 0: active session -> duration = -1 (incomplete), start = 127 min ago
     *   slot 1: Event B        -> duration = 120 min,        start = 517 min ago
     *   slot 2: Event A        -> duration = 240 min,        start = 2077 min ago
     *   slots 3..49: missing   -> duration = -1,             start = -1 */
    CHECK((f = find_feat(feats, n, "ChargeEventHistoryDuration0")) && f->value == -1.0);
    CHECK((f = find_feat(feats, n, "ChargeEventHistoryStartTime0")) && f->value == 127.0);
    CHECK((f = find_feat(feats, n, "ChargeEventHistoryDuration1")) && f->value == 120.0);
    CHECK((f = find_feat(feats, n, "ChargeEventHistoryStartTime1")) && f->value == 517.0);
    CHECK((f = find_feat(feats, n, "ChargeEventHistoryDuration2")) && f->value == 240.0);
    CHECK((f = find_feat(feats, n, "ChargeEventHistoryStartTime2")) && f->value == 2077.0);

    for (int i = 3; i < BAT_CTRL_MAX_CHARGE_EVENTS; i++) {
        char dname[48], sname[48];
        snprintf(dname, sizeof(dname), "ChargeEventHistoryDuration%d", i);
        snprintf(sname, sizeof(sname), "ChargeEventHistoryStartTime%d", i);
        CHECK((f = find_feat(feats, n, dname)) && f->value == -1.0);
        CHECK((f = find_feat(feats, n, sname)) && f->value == -1.0);
    }

    /* Daily summaries: 3 days present (0, 1, 2), days 3..29 missing (-1). */
    CHECK((f = find_feat(feats, n, "DailySummarySize")) && f->value == 3.0);

    /* Day 0 (today): hold=0, full=0, on_ac = 120 + 120 (active) = 240. */
    CHECK((f = find_feat(feats, n, "DailySummaryHoldTimeOnAc0")) && f->value == 0.0);
    CHECK((f = find_feat(feats, n, "DailySummaryTimeFullOnAc0")) && f->value == 0.0);
    CHECK((f = find_feat(feats, n, "DailySummaryTimeOnAc0")) && f->value == 240.0);

    /* Day 1 (yesterday): hold=0, full=120, on_ac=240. */
    CHECK((f = find_feat(feats, n, "DailySummaryHoldTimeOnAc1")) && f->value == 0.0);
    CHECK((f = find_feat(feats, n, "DailySummaryTimeFullOnAc1")) && f->value == 120.0);
    CHECK((f = find_feat(feats, n, "DailySummaryTimeOnAc1")) && f->value == 240.0);

    /* Day 2 (2 days ago): hold=60, full=180, on_ac=360. */
    CHECK((f = find_feat(feats, n, "DailySummaryHoldTimeOnAc2")) && f->value == 60.0);
    CHECK((f = find_feat(feats, n, "DailySummaryTimeFullOnAc2")) && f->value == 180.0);
    CHECK((f = find_feat(feats, n, "DailySummaryTimeOnAc2")) && f->value == 360.0);

    for (int d = 3; d < BAT_CTRL_MAX_RETENTION_DAYS; d++) {
        char hname[48], fname[48], aname[48];
        snprintf(hname, sizeof(hname), "DailySummaryHoldTimeOnAc%d", d);
        snprintf(fname, sizeof(fname), "DailySummaryTimeFullOnAc%d", d);
        snprintf(aname, sizeof(aname), "DailySummaryTimeOnAc%d", d);
        CHECK((f = find_feat(feats, n, hname)) && f->value == -1.0);
        CHECK((f = find_feat(feats, n, fname)) && f->value == -1.0);
        CHECK((f = find_feat(feats, n, aname)) && f->value == -1.0);
    }

    /* Verify RatioTimeFullCharge == 0 when on_ac == 0. */
    struct bat_ctrl empty;
    CHECK(bat_ctrl_init(&empty, NULL) == 0);
    n = bat_ctrl_features(&empty, &ctx, query_wall, query_boot, feats,
                          BAT_CTRL_FEATURE_COUNT);
    CHECK(n == BAT_CTRL_FEATURE_COUNT);
    CHECK((f = find_feat(feats, n, "RatioTimeFullCharge")) && f->value == 0.0);
    CHECK((f = find_feat(feats, n, "ChargeEventHistorySize")) && f->value == 0.0);
    CHECK((f = find_feat(feats, n, "DailySummarySize")) && f->value == 0.0);
    return true;
}

/* ---- 11. Export / import round trip and invalid input rejection ---- */

static bool test_export_import_roundtrip_and_validation(void)
{
    struct bat_ctrl c1;
    CHECK(setup_controller_with_history(&c1, 16, 6, 3, 1, 12, BASE_DAY_UTC, 500.0));

    /* Plug in so we also test active session round-trip. */
    struct bat_ctrl_power on = { true, false, 65.0, 65.0 };
    (void)bat_ctrl_on_power(&c1, &on, BASE_DAY_UTC + H(1), 500.0 + 3600.0);

    struct bat_history_snapshot snap1;
    bat_ctrl_export_history(&c1, &snap1);
    CHECK(snap1.event_count == 12);
    CHECK(snap1.has_active_session);
    CHECK(snap1.active_start_wall_sec == BASE_DAY_UTC + H(1));
    CHECK(snap1.day_count == 16);

    /* Import into fresh controller 30 min later (boot restarted at 10.0). */
    struct bat_ctrl c2;
    CHECK(bat_ctrl_init(&c2, NULL) == 0);
    CHECK(bat_ctrl_import_history(&c2, &snap1, &on,
                                  BASE_DAY_UTC + H(1) + M(30), 10.0) == 0);

    /* Active session offset preserves the 30 min elapsed before reboot. */
    CHECK(c2.history.has_ac_connect);
    CHECK(c2.history.ac_connect_wall_sec == BASE_DAY_UTC + H(1));
    CHECK(bat_hist_time_on_ac_sec(&c2.history, 10.0) == 16 * H(6) + M(30));

    struct bat_history_snapshot snap2;
    bat_ctrl_export_history(&c2, &snap2);
    CHECK(snap2.event_count == snap1.event_count);
    CHECK(snap2.day_count == snap1.day_count);
    CHECK(snap2.has_active_session == snap1.has_active_session);
    CHECK(snap2.active_start_wall_sec == snap1.active_start_wall_sec);
    for (size_t i = 0; i < snap1.event_count; i++) {
        CHECK(snap2.events[i].start_wall_sec == snap1.events[i].start_wall_sec);
        CHECK(snap2.events[i].duration_sec == snap1.events[i].duration_sec);
    }
    for (size_t i = 0; i < snap1.day_count; i++) {
        CHECK(snap2.days[i].day_utc_sec == snap1.days[i].day_utc_sec);
        CHECK(snap2.days[i].time_on_ac_sec == snap1.days[i].time_on_ac_sec);
        CHECK(snap2.days[i].time_full_on_ac_sec == snap1.days[i].time_full_on_ac_sec);
        CHECK(snap2.days[i].hold_time_on_ac_sec == snap1.days[i].hold_time_on_ac_sec);
    }

    /* Validation rejections: must fail and leave existing history untouched. */
    struct bat_history_snapshot bad;

    /* 1) Out-of-range event_count. */
    bad = snap1;
    bad.event_count = BAT_CTRL_MAX_CHARGE_EVENTS + 1;
    CHECK(bat_ctrl_import_history(&c2, &bad, &on, BASE_DAY_UTC + H(2), 50.0) == -1);
    CHECK(c2.history.event_count == 12);

    /* 2) Out-of-range day_count. */
    bad = snap1;
    bad.day_count = (sizeof(bad.days) / sizeof(bad.days[0])) + 1;
    CHECK(bat_ctrl_import_history(&c2, &bad, &on, BASE_DAY_UTC + H(2), 50.0) == -1);

    /* 3) Non-monotonic event timestamps. */
    bad = snap1;
    bad.events[1].start_wall_sec = bad.events[0].start_wall_sec;
    CHECK(bat_ctrl_import_history(&c2, &bad, &on, BASE_DAY_UTC + H(2), 50.0) == -1);

    /* 4) Negative event duration. */
    bad = snap1;
    bad.events[0].duration_sec = -900;
    CHECK(bat_ctrl_import_history(&c2, &bad, &on, BASE_DAY_UTC + H(2), 50.0) == -1);

    /* 5) Day not aligned to UTC midnight. */
    bad = snap1;
    bad.days[0].day_utc_sec += 60;
    CHECK(bat_ctrl_import_history(&c2, &bad, &on, BASE_DAY_UTC + H(2), 50.0) == -1);

    /* 6) Day duration > 24 h. */
    bad = snap1;
    bad.days[0].time_on_ac_sec = BAT_CTRL_DAY_SEC + 900;
    CHECK(bat_ctrl_import_history(&c2, &bad, &on, BASE_DAY_UTC + H(2), 50.0) == -1);

    /* 7) Flat int64_t serialize / deserialize round-trip & validation. */
    int64_t buf[BAT_CTRL_SERIAL_MAX_I64];
    CHECK(bat_ctrl_serialize_history(&c1, buf, 4) == -1);
    int nw = bat_ctrl_serialize_history(&c1, buf, BAT_CTRL_SERIAL_MAX_I64);
    CHECK(nw == 5 + 2 * 12 + 4 * 16);

    struct bat_ctrl c3;
    CHECK(bat_ctrl_init(&c3, NULL) == 0);
    CHECK(bat_ctrl_deserialize_history(&c3, buf, (size_t)nw, &on,
                                       BASE_DAY_UTC + H(1) + M(30), 10.0) == 0);
    CHECK(c3.history.event_count == 12);
    CHECK(c3.history.day_count == 16);
    CHECK(c3.history.has_ac_connect);
    CHECK(bat_hist_time_on_ac_sec(&c3.history, 10.0) == 16 * H(6) + M(30));

    /* Reject bad version, truncated buffer, and out-of-range event count. */
    buf[0] = 99;
    CHECK(bat_ctrl_deserialize_history(&c3, buf, (size_t)nw, &on,
                                       BASE_DAY_UTC + H(2), 50.0) == -1);
    buf[0] = BAT_CTRL_SERIAL_VERSION;
    CHECK(bat_ctrl_deserialize_history(&c3, buf, (size_t)(nw - 1), &on,
                                       BASE_DAY_UTC + H(2), 50.0) == -1);
    buf[3] = BAT_CTRL_MAX_CHARGE_EVENTS + 1;
    CHECK(bat_ctrl_deserialize_history(&c3, buf, (size_t)nw, &on,
                                       BASE_DAY_UTC + H(2), 50.0) == -1);

    /* 8) Invalid config rejected by bat_ctrl_init; default context sanity. */
    struct bat_ctrl_config bad_cfg;
    bat_ctrl_default_config(&bad_cfg);
    bad_cfg.hold_percent = 100;
    CHECK(bat_ctrl_init(&c2, &bad_cfg) == -1);
    bat_ctrl_default_config(&bad_cfg);
    bad_cfg.min_probability = 1.5;
    CHECK(bat_ctrl_init(&c2, &bad_cfg) == -1);

    struct bat_ctrl_context dctx;
    bat_ctrl_default_context(&dctx);
    CHECK(dctx.mode == BAT_CTRL_MODE_LAPTOP && dctx.screen_brightness_pct == 0);
    return true;
}

int main(void)
{
    run_test("learning_gate_days", test_learning_gate_days);
    run_test("learning_gate_ratio", test_learning_gate_ratio);
    run_test("cdf_9_buckets_and_sustain", test_cdf_buckets);
    run_test("percentile_cap_and_no_pushout", test_percentile_cap_and_no_pushout);
    run_test("guard_96_and_full_downgrade", test_96_guard_and_full_downgrade);
    run_test("charge_now_user_canceled", test_charge_now);
    run_test("shutdown_clears_state", test_shutdown);
    run_test("support_user_disabled_and_suspend", test_support_user_disabled_and_suspend);
    run_test("history_flooring_cap_retention", test_history_flooring_cap_retention);
    run_test("percentile_edges_and_clock_step", test_percentile_and_clock_step);
    run_test("feature_vector_golden", test_feature_vector_golden);
    run_test("export_import_roundtrip_and_validation", test_export_import_roundtrip_and_validation);

    printf("SUMMARY: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
