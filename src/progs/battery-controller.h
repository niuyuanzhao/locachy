/* Dependency-free, clock-injectable port of ChromiumOS powerd's
 * ChargeHistory and AdaptiveChargingController.
 *
 * Upstream reference:
 *   chromiumos/platform2 power_manager/powerd/policy/
 *     adaptive_charging_controller.{h,cc}
 *   chromiumos/platform2 system_api/dbus/power_manager/
 *     user_charging_event.proto, charge_history_state.proto
 *
 * Pure logic: no file I/O, no sysfs, no D-Bus, no real clock calls.
 * Callers supply timestamps on two axes:
 *   - boot_sec: monotonic boot-time seconds (double, >= 0; 0 or negative means
 *     "unset" only internally; callers should pass > 0 for live events).
 *   - wall_sec: UTC epoch seconds (int64_t, >= 0).
 *
 * Slow charging (upstream SLOWCHARGE / kFinishSlowChargingDelay = 3 h) and
 * Charge Limit are intentionally omitted because LOCachy's sysfs path has no
 * EC current-limit or force-discharge control. */
#ifndef LOCACHY_BATTERY_CONTROLLER_H
#define LOCACHY_BATTERY_CONTROLLER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "battery-feature.h"

/* Upstream ChargeHistory retention and privacy-flooring constants. */
#define BAT_CTRL_MAX_RETENTION_DAYS   30
#define BAT_CTRL_MAX_CHARGE_EVENTS    50
#define BAT_CTRL_INTERVAL_SEC         (15 * 60)       /* 15 minutes */
#define BAT_CTRL_DAY_SEC              86400           /* 24 hours */
#define BAT_CTRL_RETENTION_SEC        ((int64_t)BAT_CTRL_MAX_RETENTION_DAYS * BAT_CTRL_DAY_SEC)

/* Upstream AdaptiveChargingController defaults. */
#define BAT_CTRL_DEFAULT_MIN_DAYS_HISTORY       14
#define BAT_CTRL_DEFAULT_MIN_FULL_ON_AC_RATIO   0.5
#define BAT_CTRL_DEFAULT_MAX_DELAY_PERCENTILE   0.3
#define BAT_CTRL_DEFAULT_HOLD_PERCENT           80
#define BAT_CTRL_DEFAULT_HOLD_DELTA_PERCENT     0
#define BAT_CTRL_DEFAULT_MIN_PROBABILITY        0.35
#define BAT_CTRL_DEFAULT_RECHECK_INTERVAL_SEC   (30.0 * 60.0)   /* 30 minutes */
#define BAT_CTRL_FINISH_CHARGING_DELAY_SEC      (2.0 * 3600.0)  /* 2 hours */
#define BAT_CTRL_MAX_START_PERCENT              96.0
#define BAT_CTRL_TIME_BUCKET_MIN                15

/* Exact count of features emitted by bat_ctrl_features():
 *   9 scalars + ChargeEventHistorySize + 50*(Duration + StartTime)
 *   + DailySummarySize + 30*(HoldTimeOnAc + TimeFullOnAc + TimeOnAc) = 201. */
#define BAT_CTRL_FEATURE_COUNT (9 + 1 + 2 * BAT_CTRL_MAX_CHARGE_EVENTS + 1 + 3 * BAT_CTRL_MAX_RETENTION_DAYS)

/* Flat int64_t serialization format version and worst-case element count:
 *   header(version, has_active, active_start, event_count, day_count) = 5
 *   + 2 * MAX_CHARGE_EVENTS (100) + 4 * (MAX_RETENTION_DAYS + 2) (128) = 233. */
#define BAT_CTRL_SERIAL_VERSION  1
#define BAT_CTRL_SERIAL_MAX_I64  (5 + 2 * BAT_CTRL_MAX_CHARGE_EVENTS + 4 * (BAT_CTRL_MAX_RETENTION_DAYS + 2))

/* Upstream metrics::AdaptiveChargingState (SLOWCHARGE omitted). */
enum bat_ctrl_state {
    BAT_CTRL_STATE_ACTIVE = 0,
    BAT_CTRL_STATE_INACTIVE,
    BAT_CTRL_STATE_HEURISTIC_DISABLED,
    BAT_CTRL_STATE_USER_CANCELED,
    BAT_CTRL_STATE_USER_DISABLED,
    BAT_CTRL_STATE_SHUTDOWN,
    BAT_CTRL_STATE_NOT_SUPPORTED,
};

/* Upstream UserChargingEvent::Features::DeviceMode. */
enum bat_ctrl_device_mode {
    BAT_CTRL_MODE_UNKNOWN    = 0,
    BAT_CTRL_MODE_CLOSED_LID = 1,
    BAT_CTRL_MODE_LAPTOP     = 2,
    BAT_CTRL_MODE_TABLET     = 3,
};

/* Upstream UserChargingEvent::Event::Reason. */
enum bat_ctrl_reason {
    BAT_CTRL_REASON_CHARGER_PLUGGED_IN = 1,
    BAT_CTRL_REASON_CHARGER_UNPLUGGED  = 2,
    BAT_CTRL_REASON_PERIODIC_LOG       = 3,
    BAT_CTRL_REASON_SHUTDOWN           = 4,
    BAT_CTRL_REASON_SUSPEND            = 5,
};

/* Action requested of the caller/daemon after a state update or prediction. */
enum bat_ctrl_action {
    BAT_CTRL_ACTION_NONE = 0,
    BAT_CTRL_ACTION_HOLD,
    BAT_CTRL_ACTION_RESUME,
};

/* Decision output from prediction evaluation or state transitions.
 *   action          - HOLD (apply sustain_percent), RESUME (clear sustain /
 *                     charge to full), or NONE (leave hardware as-is).
 *   sustain_percent - Target hold percent when action == HOLD, else -1.
 *   release_at      - Boot-time seconds at which to stop holding and begin
 *                     charging to full; -1.0 when unbounded (bucket 8) or no
 *                     release alarm is scheduled.
 *   target_full_at  - Boot-time seconds at which battery is expected to reach
 *                     full charge; -1.0 when unset, HUGE_VAL (INFINITY) when
 *                     unbounded (bucket 8).
 *   recheck_at      - Boot-time seconds for the next prediction recheck, or
 *                     -1.0 when no recheck alarm is running. */
struct bat_ctrl_decision {
    enum bat_ctrl_action action;
    int sustain_percent;
    double release_at;
    double target_full_at;
    double recheck_at;
};

/* Configuration parameters (upstream defaults via bat_ctrl_default_config). */
struct bat_ctrl_config {
    bool supported;
    bool enabled;
    int hold_percent;            /* (0, 100), default 80 */
    int hold_delta_percent;      /* [0, 100), default 0 */
    double min_probability;      /* [0.0, 1.0], default 0.35 */
    double max_delay_percentile; /* [0.0, 1.0], default 0.3 */
    int min_days_history;        /* >= 0, default 14 */
    double min_full_on_ac_ratio; /* [0.0, 1.0], default 0.5 */
    double recheck_interval_sec; /* > 0, default 1800.0 (30 min) */
};

/* Caller-supplied live power/battery snapshot. */
struct bat_ctrl_power {
    bool ac_connected;
    bool battery_full;
    double battery_pct;          /* Raw battery percentage [0, 100] */
    double display_battery_pct;  /* Display battery percentage [0, 100] */
};

/* Caller-supplied local-time and device-context inputs for feature extraction. */
struct bat_ctrl_context {
    int local_hour;                    /* 0..23 */
    int local_minute;                  /* 0..59 */
    int day_of_week;                   /* 0=Sun..6=Sat */
    int day_of_month;                  /* 1..31 */
    enum bat_ctrl_device_mode mode;    /* Default BAT_CTRL_MODE_LAPTOP (2) */
    int screen_brightness_pct;         /* 0..100, default 0 */
    enum bat_ctrl_reason reason;       /* CHARGER_PLUGGED_IN..SUSPEND */
};

/* Completed AC charging session (15-min floored UTC start & duration in sec). */
struct bat_charge_event {
    int64_t start_wall_sec;
    int64_t duration_sec;
};

/* Per-UTC-day summary (day_utc_sec is UTC midnight; durations 15-min floored). */
struct bat_daily_summary {
    int64_t day_utc_sec;
    int64_t time_on_ac_sec;
    int64_t time_full_on_ac_sec;
    int64_t hold_time_on_ac_sec;
};

/* Serializable snapshot of ChargeHistory for daemon persistence. */
struct bat_history_snapshot {
    size_t event_count;
    struct bat_charge_event events[BAT_CTRL_MAX_CHARGE_EVENTS];
    bool has_active_session;
    int64_t active_start_wall_sec;

    size_t day_count;
    struct bat_daily_summary days[BAT_CTRL_MAX_RETENTION_DAYS + 2];
};

/* Pure in-memory port of upstream ChargeHistory. */
struct bat_charge_history {
    bool initialized;
    bool ac_connected;

    /* Active session timestamps (0 / false when not active). */
    bool has_ac_connect;
    int64_t ac_connect_wall_sec;
    double ac_connect_boot_sec;
    double ac_connect_boot_offset_sec;

    bool has_full_charge;
    int64_t full_charge_wall_sec;
    double full_charge_boot_sec;
    double full_charge_boot_offset_sec;

    bool has_hold_charge;
    int64_t hold_charge_wall_sec;
    double hold_charge_boot_sec;

    /* Completed sessions, sorted ascending by start_wall_sec. */
    size_t event_count;
    struct bat_charge_event events[BAT_CTRL_MAX_CHARGE_EVENTS];

    /* Daily summaries, sorted ascending by day_utc_sec.
     * Capacity allows temporary headroom before retention pruning. */
    size_t day_count;
    struct bat_daily_summary days[BAT_CTRL_MAX_RETENTION_DAYS + 2];

    /* Running totals across currently retained days (15-min floored). */
    int64_t duration_on_ac_sec;
    int64_t duration_full_on_ac_sec;
    int64_t hold_duration_on_ac_sec;
};

/* Pure in-memory port of upstream AdaptiveChargingController. */
struct bat_ctrl {
    struct bat_ctrl_config cfg;
    struct bat_charge_history history;
    struct bat_ctrl_power power;

    enum bat_ctrl_state state;
    bool started;
    bool is_sustain_set;
    int sustain_percent;         /* Last active sustain upper percent, or -1 */
    int display_percent;
    bool report_charge_time;

    /* Alarm states (boot-time seconds; active flags distinguish running). */
    bool recheck_alarm_running;
    double recheck_alarm_at;
    bool charge_alarm_running;
    double charge_alarm_at;

    /* Session timing (boot-time seconds; *_valid flags replace TimeTicks()). */
    bool target_full_valid;
    bool target_full_unbounded;  /* Upstream base::TimeTicks::Max() */
    double target_full_boot_sec;

    bool hold_start_valid;
    double hold_start_boot_sec;

    bool hold_end_valid;
    double hold_end_boot_sec;

    bool charge_finished_valid;
    double charge_finished_boot_sec;

    /* Set true when the controller wants the caller to run the ML model and
     * feed the 9 probabilities back via bat_ctrl_on_prediction(). */
    bool prediction_requested;
    enum bat_ctrl_reason prediction_reason;
};

/* ---- Time & clock-jump helpers (pure functions) ---- */

/* Floor a UTC epoch timestamp or duration (seconds) to a 15-minute multiple. */
int64_t bat_hist_floor_sec(int64_t sec);

/* UTC midnight (00:00:00Z) for a UTC epoch timestamp. */
int64_t bat_hist_utc_midnight(int64_t wall_sec);

/* Pure clock-step rule matching upstream ChargeHistory::CheckAndFixTimestamp:
 * compares wall_elapsed_sec and boot_elapsed_sec (both floored to 15 min).
 * If |floor(wall_elapsed) - floor(boot_elapsed)| > 15 min, returns true and
 * sets *corrected_start_wall_sec = floor(now_wall_sec) - floor(boot_elapsed).
 * Otherwise returns false. */
bool bat_hist_check_clock_step(int64_t now_wall_sec,
                               int64_t wall_elapsed_sec,
                               double boot_elapsed_sec,
                               int64_t *corrected_start_wall_sec);

/* ---- ChargeHistory API ---- */

void bat_hist_init(struct bat_charge_history *h);

/* Synchronize history with current power/hold state and timestamps. */
void bat_hist_update(struct bat_charge_history *h,
                     const struct bat_ctrl_power *power,
                     bool adaptive_delaying_charge,
                     int64_t wall_sec,
                     double boot_sec);

/* Flush in-memory full/hold durations before suspend or shutdown. */
void bat_hist_enter_low_power(struct bat_charge_history *h,
                              int64_t wall_sec,
                              double boot_sec);

/* Run retention pruning (50/49 charge event cap and 30-day cutoff). */
void bat_hist_prune(struct bat_charge_history *h, int64_t wall_sec);

/* Explicit clock-step check (also runs automatically during updates). */
void bat_hist_check_time_change(struct bat_charge_history *h,
                                int64_t wall_sec,
                                double boot_sec);

/* Total retained durations in seconds (including active session, 15-min floored). */
int64_t bat_hist_time_on_ac_sec(const struct bat_charge_history *h, double boot_sec);
int64_t bat_hist_time_full_on_ac_sec(const struct bat_charge_history *h, double boot_sec);
int64_t bat_hist_hold_time_on_ac_sec(const struct bat_charge_history *h, double boot_sec);

/* Number of days currently tracked in time_on_ac daily summaries. */
int bat_hist_days_of_history(const struct bat_charge_history *h);

/* Percentile over completed charge event durations in seconds.
 * percentile must be in [0.0, 1.0]. Returns INT64_MAX if no completed events
 * exist or percentile == 1.0 (matches upstream base::TimeDelta::Max()).
 * Otherwise index = min(n - 1, ceil(percentile * n)) on sorted durations. */
int64_t bat_hist_duration_percentile_sec(const struct bat_charge_history *h,
                                         double percentile);

/* Export/import history for daemon persistence.
 * Export copies completed events, optional active start, and daily summaries.
 * Import validates counts, alignment, monotonicity, and ranges; returns 0 on
 * success, -1 on invalid input (leaving *h unchanged on error).
 * After a valid import, call bat_hist_finish_import() (or pass power/time to
 * bat_ctrl_import_history()) to prune to wall_sec, fill missing days to today,
 * and establish active-session boot offsets. */
void bat_hist_export(const struct bat_charge_history *h,
                     struct bat_history_snapshot *out);
int bat_hist_import(struct bat_charge_history *h,
                    const struct bat_history_snapshot *in,
                    const struct bat_ctrl_power *power,
                    bool adaptive_delaying_charge,
                    int64_t wall_sec,
                    double boot_sec);

/* ---- AdaptiveChargingController API ---- */

/* Fill cfg with upstream defaults (supported=true, enabled=true). */
void bat_ctrl_default_config(struct bat_ctrl_config *cfg);

/* Fill ctx with safe defaults (mode=LAPTOP, brightness=0, reason=PERIODIC_LOG). */
void bat_ctrl_default_context(struct bat_ctrl_context *ctx);

/* Initialize controller with cfg (NULL uses bat_ctrl_default_config).
 * Returns 0 on success, -1 if cfg values are out of valid bounds. */
int bat_ctrl_init(struct bat_ctrl *c, const struct bat_ctrl_config *cfg);

/* Import persisted ChargeHistory into the controller. Returns 0 or -1. */
int bat_ctrl_import_history(struct bat_ctrl *c,
                            const struct bat_history_snapshot *in,
                            const struct bat_ctrl_power *power,
                            int64_t wall_sec,
                            double boot_sec);

/* Export current ChargeHistory from the controller. */
void bat_ctrl_export_history(const struct bat_ctrl *c,
                             struct bat_history_snapshot *out);

/* Serialize/deserialize ChargeHistory to/from a flat int64_t array so callers
 * can persist it without struct-layout coupling.
 * Layout: [version(1), has_active(0/1), active_start_wall_sec,
 *          event_count, (start_wall_sec, duration_sec) * event_count,
 *          day_count, (day_utc_sec, on_ac, full, hold) * day_count].
 * Serialize returns element count written (<= BAT_CTRL_SERIAL_MAX_I64), or -1.
 * Deserialize validates version, length, counts, and ranges; returns 0 or -1. */
int bat_ctrl_serialize_history(const struct bat_ctrl *c,
                               int64_t *out,
                               size_t max_elems);
int bat_ctrl_deserialize_history(struct bat_ctrl *c,
                                 const int64_t *in,
                                 size_t n_elems,
                                 const struct bat_ctrl_power *power,
                                 int64_t wall_sec,
                                 double boot_sec);

/* Update runtime configuration (analogous to upstream HandlePolicyChange).
 * May stop/restart adaptive charging and return a hardware decision. */
struct bat_ctrl_decision bat_ctrl_set_config(struct bat_ctrl *c,
                                             const struct bat_ctrl_config *cfg,
                                             int64_t wall_sec,
                                             double boot_sec);

/* Supply a new power/battery snapshot (AC plug/unplug, pct, full state).
 * Updates ChargeHistory and state machine. If c->prediction_requested becomes
 * true, the caller should extract features via bat_ctrl_features(), run the
 * model, and pass the 9 outputs to bat_ctrl_on_prediction(). */
struct bat_ctrl_decision bat_ctrl_on_power(struct bat_ctrl *c,
                                           const struct bat_ctrl_power *power,
                                           int64_t wall_sec,
                                           double boot_sec);

/* Advance timers / periodic tick (e.g., every 15 min or when an alarm is due).
 * Handles charge_alarm expiry (stops hold -> RESUME), recheck_alarm expiry
 * (sets prediction_requested with PERIODIC_LOG), clock jumps, and retention
 * pruning. */
struct bat_ctrl_decision bat_ctrl_tick(struct bat_ctrl *c,
                                       int64_t wall_sec,
                                       double boot_sec);

/* Feed the ML model's 9-bucket output probabilities (or inference_done=false
 * on failure) back to the controller. Computes the hold/resume decision per
 * upstream AdaptiveChargingController::OnPredictionResponse. */
struct bat_ctrl_decision bat_ctrl_on_prediction(struct bat_ctrl *c,
                                                bool inference_done,
                                                const double probs[BAT_ADAPTIVE_OUTPUTS],
                                                int64_t wall_sec,
                                                double boot_sec);

/* User clicked "Charge to Full Now": transitions ACTIVE -> USER_CANCELED,
 * stops holding, stays inactive until the next AC plug-in. */
struct bat_ctrl_decision bat_ctrl_charge_now(struct bat_ctrl *c,
                                             int64_t wall_sec,
                                             double boot_sec);

/* Prepare for suspend: flushes history low-power durations and, if adaptive
 * charging is running, requests a synchronous SUSPEND prediction. */
struct bat_ctrl_decision bat_ctrl_on_suspend(struct bat_ctrl *c,
                                             int64_t wall_sec,
                                             double boot_sec);

/* Resume from suspend: checks clock step and prunes stale history. */
void bat_ctrl_on_resume(struct bat_ctrl *c, int64_t wall_sec, double boot_sec);

/* Enter shutdown: transitions to SHUTDOWN, clears hold state, flushes history. */
struct bat_ctrl_decision bat_ctrl_on_shutdown(struct bat_ctrl *c,
                                              int64_t wall_sec,
                                              double boot_sec);

/* Extract the 201 RankerExample features matching upstream
 * AdaptiveChargingController::UpdateAdaptiveCharging.
 *
 * Feature name storage: every emitted out[i].name points into static,
 * immutable string storage owned by battery-controller.c, so pointers remain
 * valid forever without any caller buffer or heap allocation.
 *
 * All 201 upstream features use int32_value in upstream powerd, so every
 * emitted feature has kind == BAT_FEAT_INT32.
 *
 * Returns BAT_CTRL_FEATURE_COUNT (201) on success, or -1 if out is NULL,
 * max < BAT_CTRL_FEATURE_COUNT, or inputs are NULL. */
int bat_ctrl_features(const struct bat_ctrl *c,
                      const struct bat_ctrl_context *ctx,
                      int64_t wall_sec,
                      double boot_sec,
                      struct bat_feature *out,
                      size_t max);

/* True when display_battery_pct is within/above the active hold threshold
 * (hold_percent - hold_delta_percent - 1), matching upstream AtHoldPercent. */
bool bat_ctrl_at_hold_percent(const struct bat_ctrl *c, double display_battery_pct);

/* True while the periodic recheck alarm is active (upstream IsRunning). */
bool bat_ctrl_is_running(const struct bat_ctrl *c);

/* Human-readable name for enum bat_ctrl_state (static string). */
const char *bat_ctrl_state_name(enum bat_ctrl_state state);

#endif
