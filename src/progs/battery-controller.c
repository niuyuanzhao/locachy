/* Dependency-free, clock-injectable port of ChromiumOS powerd's
 * ChargeHistory and AdaptiveChargingController.
 *
 * See battery-controller.h for API contracts and upstream references. */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "battery-controller.h"

#include <limits.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

/* ---- Static feature name tables (lifetime: entire process) ---- */

#define FEAT_EVENT_DUR(n)   "ChargeEventHistoryDuration" #n
#define FEAT_EVENT_START(n) "ChargeEventHistoryStartTime" #n
#define FEAT_DAY_HOLD(n)    "DailySummaryHoldTimeOnAc" #n
#define FEAT_DAY_FULL(n)    "DailySummaryTimeFullOnAc" #n
#define FEAT_DAY_ON_AC(n)   "DailySummaryTimeOnAc" #n

static const char *const k_event_dur_names[BAT_CTRL_MAX_CHARGE_EVENTS] = {
    FEAT_EVENT_DUR(0),  FEAT_EVENT_DUR(1),  FEAT_EVENT_DUR(2),  FEAT_EVENT_DUR(3),  FEAT_EVENT_DUR(4),
    FEAT_EVENT_DUR(5),  FEAT_EVENT_DUR(6),  FEAT_EVENT_DUR(7),  FEAT_EVENT_DUR(8),  FEAT_EVENT_DUR(9),
    FEAT_EVENT_DUR(10), FEAT_EVENT_DUR(11), FEAT_EVENT_DUR(12), FEAT_EVENT_DUR(13), FEAT_EVENT_DUR(14),
    FEAT_EVENT_DUR(15), FEAT_EVENT_DUR(16), FEAT_EVENT_DUR(17), FEAT_EVENT_DUR(18), FEAT_EVENT_DUR(19),
    FEAT_EVENT_DUR(20), FEAT_EVENT_DUR(21), FEAT_EVENT_DUR(22), FEAT_EVENT_DUR(23), FEAT_EVENT_DUR(24),
    FEAT_EVENT_DUR(25), FEAT_EVENT_DUR(26), FEAT_EVENT_DUR(27), FEAT_EVENT_DUR(28), FEAT_EVENT_DUR(29),
    FEAT_EVENT_DUR(30), FEAT_EVENT_DUR(31), FEAT_EVENT_DUR(32), FEAT_EVENT_DUR(33), FEAT_EVENT_DUR(34),
    FEAT_EVENT_DUR(35), FEAT_EVENT_DUR(36), FEAT_EVENT_DUR(37), FEAT_EVENT_DUR(38), FEAT_EVENT_DUR(39),
    FEAT_EVENT_DUR(40), FEAT_EVENT_DUR(41), FEAT_EVENT_DUR(42), FEAT_EVENT_DUR(43), FEAT_EVENT_DUR(44),
    FEAT_EVENT_DUR(45), FEAT_EVENT_DUR(46), FEAT_EVENT_DUR(47), FEAT_EVENT_DUR(48), FEAT_EVENT_DUR(49),
};

static const char *const k_event_start_names[BAT_CTRL_MAX_CHARGE_EVENTS] = {
    FEAT_EVENT_START(0),  FEAT_EVENT_START(1),  FEAT_EVENT_START(2),  FEAT_EVENT_START(3),  FEAT_EVENT_START(4),
    FEAT_EVENT_START(5),  FEAT_EVENT_START(6),  FEAT_EVENT_START(7),  FEAT_EVENT_START(8),  FEAT_EVENT_START(9),
    FEAT_EVENT_START(10), FEAT_EVENT_START(11), FEAT_EVENT_START(12), FEAT_EVENT_START(13), FEAT_EVENT_START(14),
    FEAT_EVENT_START(15), FEAT_EVENT_START(16), FEAT_EVENT_START(17), FEAT_EVENT_START(18), FEAT_EVENT_START(19),
    FEAT_EVENT_START(20), FEAT_EVENT_START(21), FEAT_EVENT_START(22), FEAT_EVENT_START(23), FEAT_EVENT_START(24),
    FEAT_EVENT_START(25), FEAT_EVENT_START(26), FEAT_EVENT_START(27), FEAT_EVENT_START(28), FEAT_EVENT_START(29),
    FEAT_EVENT_START(30), FEAT_EVENT_START(31), FEAT_EVENT_START(32), FEAT_EVENT_START(33), FEAT_EVENT_START(34),
    FEAT_EVENT_START(35), FEAT_EVENT_START(36), FEAT_EVENT_START(37), FEAT_EVENT_START(38), FEAT_EVENT_START(39),
    FEAT_EVENT_START(40), FEAT_EVENT_START(41), FEAT_EVENT_START(42), FEAT_EVENT_START(43), FEAT_EVENT_START(44),
    FEAT_EVENT_START(45), FEAT_EVENT_START(46), FEAT_EVENT_START(47), FEAT_EVENT_START(48), FEAT_EVENT_START(49),
};

static const char *const k_day_hold_names[BAT_CTRL_MAX_RETENTION_DAYS] = {
    FEAT_DAY_HOLD(0),  FEAT_DAY_HOLD(1),  FEAT_DAY_HOLD(2),  FEAT_DAY_HOLD(3),  FEAT_DAY_HOLD(4),
    FEAT_DAY_HOLD(5),  FEAT_DAY_HOLD(6),  FEAT_DAY_HOLD(7),  FEAT_DAY_HOLD(8),  FEAT_DAY_HOLD(9),
    FEAT_DAY_HOLD(10), FEAT_DAY_HOLD(11), FEAT_DAY_HOLD(12), FEAT_DAY_HOLD(13), FEAT_DAY_HOLD(14),
    FEAT_DAY_HOLD(15), FEAT_DAY_HOLD(16), FEAT_DAY_HOLD(17), FEAT_DAY_HOLD(18), FEAT_DAY_HOLD(19),
    FEAT_DAY_HOLD(20), FEAT_DAY_HOLD(21), FEAT_DAY_HOLD(22), FEAT_DAY_HOLD(23), FEAT_DAY_HOLD(24),
    FEAT_DAY_HOLD(25), FEAT_DAY_HOLD(26), FEAT_DAY_HOLD(27), FEAT_DAY_HOLD(28), FEAT_DAY_HOLD(29),
};

static const char *const k_day_full_names[BAT_CTRL_MAX_RETENTION_DAYS] = {
    FEAT_DAY_FULL(0),  FEAT_DAY_FULL(1),  FEAT_DAY_FULL(2),  FEAT_DAY_FULL(3),  FEAT_DAY_FULL(4),
    FEAT_DAY_FULL(5),  FEAT_DAY_FULL(6),  FEAT_DAY_FULL(7),  FEAT_DAY_FULL(8),  FEAT_DAY_FULL(9),
    FEAT_DAY_FULL(10), FEAT_DAY_FULL(11), FEAT_DAY_FULL(12), FEAT_DAY_FULL(13), FEAT_DAY_FULL(14),
    FEAT_DAY_FULL(15), FEAT_DAY_FULL(16), FEAT_DAY_FULL(17), FEAT_DAY_FULL(18), FEAT_DAY_FULL(19),
    FEAT_DAY_FULL(20), FEAT_DAY_FULL(21), FEAT_DAY_FULL(22), FEAT_DAY_FULL(23), FEAT_DAY_FULL(24),
    FEAT_DAY_FULL(25), FEAT_DAY_FULL(26), FEAT_DAY_FULL(27), FEAT_DAY_FULL(28), FEAT_DAY_FULL(29),
};

static const char *const k_day_on_ac_names[BAT_CTRL_MAX_RETENTION_DAYS] = {
    FEAT_DAY_ON_AC(0),  FEAT_DAY_ON_AC(1),  FEAT_DAY_ON_AC(2),  FEAT_DAY_ON_AC(3),  FEAT_DAY_ON_AC(4),
    FEAT_DAY_ON_AC(5),  FEAT_DAY_ON_AC(6),  FEAT_DAY_ON_AC(7),  FEAT_DAY_ON_AC(8),  FEAT_DAY_ON_AC(9),
    FEAT_DAY_ON_AC(10), FEAT_DAY_ON_AC(11), FEAT_DAY_ON_AC(12), FEAT_DAY_ON_AC(13), FEAT_DAY_ON_AC(14),
    FEAT_DAY_ON_AC(15), FEAT_DAY_ON_AC(16), FEAT_DAY_ON_AC(17), FEAT_DAY_ON_AC(18), FEAT_DAY_ON_AC(19),
    FEAT_DAY_ON_AC(20), FEAT_DAY_ON_AC(21), FEAT_DAY_ON_AC(22), FEAT_DAY_ON_AC(23), FEAT_DAY_ON_AC(24),
    FEAT_DAY_ON_AC(25), FEAT_DAY_ON_AC(26), FEAT_DAY_ON_AC(27), FEAT_DAY_ON_AC(28), FEAT_DAY_ON_AC(29),
};

#undef FEAT_EVENT_DUR
#undef FEAT_EVENT_START
#undef FEAT_DAY_HOLD
#undef FEAT_DAY_FULL
#undef FEAT_DAY_ON_AC

/* ---- Pure time helpers ---- */

/* Floor toward -infinity to a multiple of step (step > 0). */
static int64_t floor_multiple_i64(int64_t v, int64_t step)
{
    int64_t r = v % step;
    if (r < 0)
        r += step;
    return v - r;
}

/* Upstream base::TimeDelta::FloorToMultiple uses truncation toward zero
 * (C++ delta - (delta % interval)) for durations; keep floor-toward-zero for
 * durations and floor-toward-neg-inf for epoch timestamps. For non-negative
 * values both agree; for negative durations (e.g., backward wall jumps),
 * truncation toward zero matches base::TimeDelta::FloorToMultiple exactly. */
static int64_t trunc_multiple_i64(int64_t v, int64_t step)
{
    return v - (v % step);
}

/* Floor a double duration (seconds) toward zero to a 15-min multiple. */
static int64_t floor_interval_double(double sec)
{
    if (!isfinite(sec))
        return 0;
    double buckets = trunc(sec / (double)BAT_CTRL_INTERVAL_SEC);
    if (buckets > (double)(INT64_MAX / BAT_CTRL_INTERVAL_SEC))
        return (INT64_MAX / BAT_CTRL_INTERVAL_SEC) * BAT_CTRL_INTERVAL_SEC;
    if (buckets < (double)(INT64_MIN / BAT_CTRL_INTERVAL_SEC))
        return (INT64_MIN / BAT_CTRL_INTERVAL_SEC) * BAT_CTRL_INTERVAL_SEC;
    return (int64_t)buckets * BAT_CTRL_INTERVAL_SEC;
}

int64_t bat_hist_floor_sec(int64_t sec)
{
    return floor_multiple_i64(sec, BAT_CTRL_INTERVAL_SEC);
}

int64_t bat_hist_utc_midnight(int64_t wall_sec)
{
    return floor_multiple_i64(wall_sec, BAT_CTRL_DAY_SEC);
}

bool bat_hist_check_clock_step(int64_t now_wall_sec,
                               int64_t wall_elapsed_sec,
                               double boot_elapsed_sec,
                               int64_t *corrected_start_wall_sec)
{
    int64_t dur = trunc_multiple_i64(wall_elapsed_sec, BAT_CTRL_INTERVAL_SEC);
    int64_t ticks_dur = floor_interval_double(boot_elapsed_sec);
    int64_t diff = dur >= ticks_dur ? dur - ticks_dur : ticks_dur - dur;
    if (diff > BAT_CTRL_INTERVAL_SEC) {
        if (corrected_start_wall_sec)
            *corrected_start_wall_sec = bat_hist_floor_sec(now_wall_sec) - ticks_dur;
        return true;
    }
    return false;
}

/* ---- Internal ChargeHistory helpers ---- */

enum hist_day_field {
    HIST_FIELD_ON_AC = 0,
    HIST_FIELD_FULL,
    HIST_FIELD_HOLD,
};

static int64_t *day_field_ptr(struct bat_daily_summary *d, enum hist_day_field f)
{
    switch (f) {
    case HIST_FIELD_ON_AC: return &d->time_on_ac_sec;
    case HIST_FIELD_FULL:  return &d->time_full_on_ac_sec;
    case HIST_FIELD_HOLD:  return &d->hold_time_on_ac_sec;
    }
    return &d->time_on_ac_sec;
}

static int64_t *hist_total_ptr(struct bat_charge_history *h, enum hist_day_field f)
{
    switch (f) {
    case HIST_FIELD_ON_AC: return &h->duration_on_ac_sec;
    case HIST_FIELD_FULL:  return &h->duration_full_on_ac_sec;
    case HIST_FIELD_HOLD:  return &h->hold_duration_on_ac_sec;
    }
    return &h->duration_on_ac_sec;
}

/* Remove oldest daily summaries strictly older than wall_sec - kRetentionDays.
 * Matches ChargeHistory::RemoveOldChargeDays (note: upstream iterates rbegin
 * due to a bug when entries are sorted ascending; here we always remove all
 * entries strictly older than cutoff so old days are pruned properly). */
static void hist_remove_old_days(struct bat_charge_history *h, int64_t wall_sec)
{
    int64_t cutoff = wall_sec - BAT_CTRL_RETENTION_SEC;
    size_t dst = 0;
    for (size_t i = 0; i < h->day_count; i++) {
        if (h->days[i].day_utc_sec < cutoff) {
            h->duration_on_ac_sec -= h->days[i].time_on_ac_sec;
            h->duration_full_on_ac_sec -= h->days[i].time_full_on_ac_sec;
            h->hold_duration_on_ac_sec -= h->days[i].hold_time_on_ac_sec;
        } else {
            if (dst != i)
                h->days[dst] = h->days[i];
            dst++;
        }
    }
    h->day_count = dst;
    if (h->duration_on_ac_sec < 0)
        h->duration_on_ac_sec = 0;
    if (h->duration_full_on_ac_sec < 0)
        h->duration_full_on_ac_sec = 0;
    if (h->hold_duration_on_ac_sec < 0)
        h->hold_duration_on_ac_sec = 0;
}

/* Find or insert a day entry in sorted order. If full, drops the oldest day. */
static struct bat_daily_summary *hist_get_or_insert_day(struct bat_charge_history *h,
                                                        int64_t day_utc_sec)
{
    const size_t cap = sizeof(h->days) / sizeof(h->days[0]);
    size_t pos = 0;
    while (pos < h->day_count && h->days[pos].day_utc_sec < day_utc_sec)
        pos++;

    if (pos < h->day_count && h->days[pos].day_utc_sec == day_utc_sec)
        return &h->days[pos];

    if (h->day_count >= cap) {
        if (pos == 0)
            return NULL; /* Older than everything we can hold. */
        h->duration_on_ac_sec -= h->days[0].time_on_ac_sec;
        h->duration_full_on_ac_sec -= h->days[0].time_full_on_ac_sec;
        h->hold_duration_on_ac_sec -= h->days[0].hold_time_on_ac_sec;
        memmove(&h->days[0], &h->days[1], (h->day_count - 1) * sizeof(h->days[0]));
        h->day_count--;
        pos--;
    }

    if (pos < h->day_count) {
        memmove(&h->days[pos + 1], &h->days[pos],
                (h->day_count - pos) * sizeof(h->days[0]));
    }
    h->days[pos] = (struct bat_daily_summary){
        .day_utc_sec = day_utc_sec,
        .time_on_ac_sec = 0,
        .time_full_on_ac_sec = 0,
        .hold_time_on_ac_sec = 0,
    };
    h->day_count++;
    return &h->days[pos];
}

/* Matches ChargeHistory::AddZeroDurationChargeDays: appends zero-filled days
 * from (latest + 1 day) through today's UTC midnight (or just today if empty),
 * bounded to the retention window so huge forward jumps don't loop forever. */
static void hist_add_zero_days(struct bat_charge_history *h, int64_t wall_sec)
{
    int64_t today = bat_hist_utc_midnight(wall_sec);
    int64_t min_day = bat_hist_utc_midnight(wall_sec - BAT_CTRL_RETENTION_SEC);
    int64_t date;

    if (h->day_count == 0) {
        date = today;
    } else {
        date = h->days[h->day_count - 1].day_utc_sec + BAT_CTRL_DAY_SEC;
        if (date < min_day)
            date = min_day;
    }

    for (; date <= today; date += BAT_CTRL_DAY_SEC) {
        (void)hist_get_or_insert_day(h, date);
    }
    hist_remove_old_days(h, wall_sec);
}

/* Matches ChargeHistory::RecordDurations: distributes [start_wall_sec, wall_sec]
 * across UTC days, floors each day's accumulated duration to 15 min, clamps to
 * 1 day, and keeps running totals in sync. */
static void hist_record_durations(struct bat_charge_history *h,
                                  enum hist_day_field field,
                                  int64_t start_wall_sec,
                                  int64_t wall_sec)
{
    if (start_wall_sec >= wall_sec)
        return;

    int64_t min_day = bat_hist_utc_midnight(wall_sec - BAT_CTRL_RETENTION_SEC);
    int64_t date = bat_hist_utc_midnight(start_wall_sec);
    if (date < min_day)
        date = min_day;

    int64_t *total = hist_total_ptr(h, field);
    while (date < wall_sec) {
        struct bat_daily_summary *d = hist_get_or_insert_day(h, date);
        if (d) {
            int64_t tomorrow = date + BAT_CTRL_DAY_SEC;
            int64_t s = start_wall_sec > date ? start_wall_sec : date;
            int64_t e = tomorrow > wall_sec ? wall_sec : tomorrow;
            int64_t dur = e > s ? e - s : 0;

            int64_t *slot = day_field_ptr(d, field);
            *total -= *slot;
            int64_t updated = bat_hist_floor_sec(*slot + dur);
            if (updated < 0)
                updated = 0;
            if (updated > BAT_CTRL_DAY_SEC)
                updated = BAT_CTRL_DAY_SEC;
            *slot = updated;
            *total += *slot;
        }
        date += BAT_CTRL_DAY_SEC;
    }
}

/* Remove any completed charge event matching start_wall_sec. */
static void hist_erase_event_at(struct bat_charge_history *h, int64_t start_wall_sec)
{
    for (size_t i = 0; i < h->event_count; i++) {
        if (h->events[i].start_wall_sec == start_wall_sec) {
            memmove(&h->events[i], &h->events[i + 1],
                    (h->event_count - i - 1) * sizeof(h->events[0]));
            h->event_count--;
            return;
        }
    }
}

/* Matches ChargeHistory::RemoveOldChargeEvents: caps completed events to 50
 * (or 49 while an AC session is active) and drops events whose end time is
 * strictly before wall_sec - kRetentionDays. */
static void hist_remove_old_events(struct bat_charge_history *h, int64_t wall_sec)
{
    size_t max_ev = BAT_CTRL_MAX_CHARGE_EVENTS;
    if (h->has_ac_connect)
        max_ev--;

    if (h->event_count > max_ev) {
        size_t drop = h->event_count - max_ev;
        memmove(&h->events[0], &h->events[drop],
                max_ev * sizeof(h->events[0]));
        h->event_count = max_ev;
    }

    int64_t cutoff = wall_sec - BAT_CTRL_RETENTION_SEC;
    size_t dst = 0;
    for (size_t i = 0; i < h->event_count; i++) {
        if (h->events[i].start_wall_sec + h->events[i].duration_sec < cutoff)
            continue;
        if (dst != i)
            h->events[dst] = h->events[i];
        dst++;
    }
    h->event_count = dst;
}

/* Insert or overwrite a completed event in ascending start_wall_sec order. */
static void hist_put_event(struct bat_charge_history *h,
                           int64_t start_wall_sec,
                           int64_t duration_sec)
{
    if (duration_sec < 0)
        duration_sec = 0;
    duration_sec = bat_hist_floor_sec(duration_sec);

    size_t pos = 0;
    while (pos < h->event_count && h->events[pos].start_wall_sec < start_wall_sec)
        pos++;

    if (pos < h->event_count && h->events[pos].start_wall_sec == start_wall_sec) {
        h->events[pos].duration_sec = duration_sec;
        return;
    }

    if (h->event_count >= BAT_CTRL_MAX_CHARGE_EVENTS) {
        if (pos == 0)
            return;
        memmove(&h->events[0], &h->events[1],
                (h->event_count - 1) * sizeof(h->events[0]));
        h->event_count--;
        pos--;
    }

    if (pos < h->event_count) {
        memmove(&h->events[pos + 1], &h->events[pos],
                (h->event_count - pos) * sizeof(h->events[0]));
    }
    h->events[pos] = (struct bat_charge_event){
        .start_wall_sec = start_wall_sec,
        .duration_sec = duration_sec,
    };
    h->event_count++;
}

/* Matches ChargeHistory::DurationForDay (used when projecting active session
 * durations onto daily summaries for feature extraction). */
static int64_t hist_duration_for_day(bool active,
                                     int64_t start_wall_sec,
                                     int64_t day_start_sec,
                                     int64_t now_wall_sec)
{
    int64_t day_end = day_start_sec + BAT_CTRL_DAY_SEC;
    if (!active || start_wall_sec > day_end)
        return 0;

    int64_t now = bat_hist_floor_sec(now_wall_sec);
    int64_t end = now < day_end ? now : day_end;
    int64_t start = start_wall_sec > day_start_sec ? start_wall_sec : day_start_sec;
    return end > start ? end - start : 0;
}

/* Matches ChargeHistory::UpdateHistory (AC connect/disconnect transitions). */
static void hist_update_ac_transition(struct bat_charge_history *h,
                                      bool ac_connected,
                                      int64_t wall_sec,
                                      double boot_sec)
{
    int64_t now_floored = bat_hist_floor_sec(wall_sec);
    h->ac_connected = ac_connected;

    if (ac_connected) {
        if (h->has_ac_connect)
            return;

        h->has_ac_connect = true;
        h->ac_connect_wall_sec = now_floored;
        h->ac_connect_boot_sec = boot_sec;
        h->ac_connect_boot_offset_sec = 0.0;

        /* Overwrite any existing completed event colliding on the same 15-min bucket. */
        hist_erase_event_at(h, h->ac_connect_wall_sec);
        hist_remove_old_events(h, wall_sec);
        return;
    }

    if (!h->has_ac_connect)
        return;

    if (h->has_full_charge) {
        hist_record_durations(h, HIST_FIELD_FULL, h->full_charge_wall_sec, wall_sec);
    }
    if (h->has_hold_charge) {
        hist_record_durations(h, HIST_FIELD_HOLD, h->hold_charge_wall_sec, wall_sec);
    }
    hist_record_durations(h, HIST_FIELD_ON_AC, h->ac_connect_wall_sec, wall_sec);

    h->has_full_charge = false;
    h->full_charge_wall_sec = 0;
    h->full_charge_boot_sec = 0.0;
    h->full_charge_boot_offset_sec = 0.0;

    h->has_hold_charge = false;
    h->hold_charge_wall_sec = 0;
    h->hold_charge_boot_sec = 0.0;

    double elapsed = (boot_sec - h->ac_connect_boot_sec) + h->ac_connect_boot_offset_sec;
    int64_t ticks_dur = floor_interval_double(elapsed);
    if (ticks_dur < 0)
        ticks_dur = 0;

    int64_t start_wall = h->ac_connect_wall_sec;
    h->has_ac_connect = false;
    h->ac_connect_wall_sec = 0;
    h->ac_connect_boot_sec = 0.0;
    h->ac_connect_boot_offset_sec = 0.0;

    hist_put_event(h, start_wall, ticks_dur);
    hist_remove_old_events(h, wall_sec);
}

/* ---- Public ChargeHistory API ---- */

void bat_hist_init(struct bat_charge_history *h)
{
    if (!h)
        return;
    memset(h, 0, sizeof(*h));
}

void bat_hist_check_time_change(struct bat_charge_history *h,
                                int64_t wall_sec,
                                double boot_sec)
{
    if (!h || !h->has_ac_connect)
        return;

    int64_t now = bat_hist_floor_sec(wall_sec);
    int64_t corrected = 0;
    if (bat_hist_check_clock_step(wall_sec,
                                  now - h->ac_connect_wall_sec,
                                  (boot_sec - h->ac_connect_boot_sec) +
                                      h->ac_connect_boot_offset_sec,
                                  &corrected)) {
        h->ac_connect_wall_sec = corrected;
        hist_add_zero_days(h, wall_sec);
    }

    if (h->has_full_charge) {
        if (bat_hist_check_clock_step(wall_sec,
                                      now - h->full_charge_wall_sec,
                                      (boot_sec - h->full_charge_boot_sec) +
                                          h->full_charge_boot_offset_sec,
                                      &corrected)) {
            h->full_charge_wall_sec = corrected;
        }
    }

    if (h->has_hold_charge) {
        if (bat_hist_check_clock_step(wall_sec,
                                      now - h->hold_charge_wall_sec,
                                      boot_sec - h->hold_charge_boot_sec,
                                      &corrected)) {
            h->hold_charge_wall_sec = corrected;
        }
    }
}

void bat_hist_update(struct bat_charge_history *h,
                     const struct bat_ctrl_power *power,
                     bool adaptive_delaying_charge,
                     int64_t wall_sec,
                     double boot_sec)
{
    if (!h || !power)
        return;

    int64_t now_floored = bat_hist_floor_sec(wall_sec);

    if (!h->initialized) {
        h->initialized = true;
        if (power->ac_connected && power->battery_full && !h->has_full_charge) {
            if (!h->has_ac_connect) {
                h->has_full_charge = true;
                h->full_charge_wall_sec = now_floored;
                h->full_charge_boot_sec = boot_sec;
                h->full_charge_boot_offset_sec = 0.0;
            } else {
                h->has_full_charge = true;
                h->full_charge_wall_sec = h->ac_connect_wall_sec;
                h->full_charge_boot_sec = h->ac_connect_boot_sec;
                h->full_charge_boot_offset_sec = h->ac_connect_boot_offset_sec;
            }
        }
        if (adaptive_delaying_charge) {
            h->has_hold_charge = true;
            h->hold_charge_wall_sec = now_floored;
            h->hold_charge_boot_sec = boot_sec;
        }
        hist_add_zero_days(h, wall_sec);
        hist_update_ac_transition(h, power->ac_connected, wall_sec, boot_sec);
        hist_remove_old_events(h, wall_sec);
        return;
    }

    if (power->ac_connected && power->battery_full && !h->has_full_charge) {
        h->has_full_charge = true;
        h->full_charge_wall_sec = now_floored;
        h->full_charge_boot_sec = boot_sec;
        h->full_charge_boot_offset_sec = 0.0;
    }

    if (adaptive_delaying_charge && !h->has_hold_charge) {
        h->has_hold_charge = true;
        h->hold_charge_wall_sec = now_floored;
        h->hold_charge_boot_sec = boot_sec;
    } else if (!adaptive_delaying_charge && h->has_hold_charge) {
        hist_record_durations(h, HIST_FIELD_HOLD, h->hold_charge_wall_sec, wall_sec);
        h->has_hold_charge = false;
        h->hold_charge_wall_sec = 0;
        h->hold_charge_boot_sec = 0.0;
    }

    bat_hist_check_time_change(h, wall_sec, boot_sec);
    hist_add_zero_days(h, wall_sec);

    if (power->ac_connected == h->ac_connected)
        return;

    hist_update_ac_transition(h, power->ac_connected, wall_sec, boot_sec);
}

void bat_hist_enter_low_power(struct bat_charge_history *h,
                              int64_t wall_sec,
                              double boot_sec)
{
    if (!h || !h->initialized)
        return;

    bat_hist_check_time_change(h, wall_sec, boot_sec);
    int64_t now_floored = bat_hist_floor_sec(wall_sec);

    if (h->has_full_charge) {
        hist_record_durations(h, HIST_FIELD_FULL, h->full_charge_wall_sec, wall_sec);
        h->full_charge_wall_sec = now_floored;
        h->full_charge_boot_sec = boot_sec;
        h->full_charge_boot_offset_sec = 0.0;
    }
    if (h->has_hold_charge) {
        hist_record_durations(h, HIST_FIELD_HOLD, h->hold_charge_wall_sec, wall_sec);
        h->hold_charge_wall_sec = now_floored;
        h->hold_charge_boot_sec = boot_sec;
    }
}

void bat_hist_prune(struct bat_charge_history *h, int64_t wall_sec)
{
    if (!h)
        return;
    hist_remove_old_events(h, wall_sec);
    hist_remove_old_days(h, wall_sec);
}

int64_t bat_hist_time_on_ac_sec(const struct bat_charge_history *h, double boot_sec)
{
    if (!h)
        return 0;
    double total = (double)h->duration_on_ac_sec;
    if (h->has_ac_connect) {
        double add = (boot_sec - h->ac_connect_boot_sec) + h->ac_connect_boot_offset_sec;
        if (add > 0.0)
            total += add;
    }
    int64_t floored = floor_interval_double(total);
    return floored > 0 ? floored : 0;
}

int64_t bat_hist_time_full_on_ac_sec(const struct bat_charge_history *h, double boot_sec)
{
    if (!h)
        return 0;
    double total = (double)h->duration_full_on_ac_sec;
    if (h->has_full_charge) {
        double add = (boot_sec - h->full_charge_boot_sec) + h->full_charge_boot_offset_sec;
        if (add > 0.0)
            total += add;
    }
    int64_t floored = floor_interval_double(total);
    return floored > 0 ? floored : 0;
}

int64_t bat_hist_hold_time_on_ac_sec(const struct bat_charge_history *h, double boot_sec)
{
    if (!h)
        return 0;
    double total = (double)h->hold_duration_on_ac_sec;
    if (h->has_hold_charge) {
        double add = boot_sec - h->hold_charge_boot_sec;
        if (add > 0.0)
            total += add;
    }
    int64_t floored = floor_interval_double(total);
    return floored > 0 ? floored : 0;
}

int bat_hist_days_of_history(const struct bat_charge_history *h)
{
    if (!h)
        return 0;
    return (int)h->day_count;
}

static int cmp_i64(const void *a, const void *b)
{
    int64_t va = *(const int64_t *)a;
    int64_t vb = *(const int64_t *)b;
    return (va > vb) - (va < vb);
}

int64_t bat_hist_duration_percentile_sec(const struct bat_charge_history *h,
                                         double percentile)
{
    if (!h || !isfinite(percentile) || percentile < 0.0 || percentile > 1.0)
        return INT64_MAX;
    if (h->event_count == 0 || percentile == 1.0)
        return INT64_MAX;

    int64_t sorted[BAT_CTRL_MAX_CHARGE_EVENTS];
    size_t n = h->event_count;
    for (size_t i = 0; i < n; i++)
        sorted[i] = h->events[i].duration_sec;

    qsort(sorted, n, sizeof(sorted[0]), cmp_i64);

    size_t idx = (size_t)ceil(percentile * (double)n);
    if (idx >= n)
        idx = n - 1;
    return sorted[idx];
}

void bat_hist_export(const struct bat_charge_history *h,
                     struct bat_history_snapshot *out)
{
    if (!out)
        return;
    memset(out, 0, sizeof(*out));
    if (!h)
        return;

    out->event_count = h->event_count;
    if (h->event_count > 0) {
        memcpy(out->events, h->events,
               h->event_count * sizeof(out->events[0]));
    }
    out->has_active_session = h->has_ac_connect;
    out->active_start_wall_sec = h->has_ac_connect ? h->ac_connect_wall_sec : 0;

    out->day_count = h->day_count;
    if (h->day_count > 0) {
        memcpy(out->days, h->days,
               h->day_count * sizeof(out->days[0]));
    }
}

int bat_hist_import(struct bat_charge_history *h,
                    const struct bat_history_snapshot *in,
                    const struct bat_ctrl_power *power,
                    bool adaptive_delaying_charge,
                    int64_t wall_sec,
                    double boot_sec)
{
    if (!h || !in || !power || wall_sec < 0 || !isfinite(boot_sec))
        return -1;

    const size_t max_days = sizeof(in->days) / sizeof(in->days[0]);
    if (in->event_count > BAT_CTRL_MAX_CHARGE_EVENTS)
        return -1;
    if (in->day_count > max_days)
        return -1;

    /* Validate completed events: non-negative start and duration, strictly
     * increasing start times, and reasonable bounds. */
    for (size_t i = 0; i < in->event_count; i++) {
        int64_t st = in->events[i].start_wall_sec;
        int64_t dur = in->events[i].duration_sec;
        if (st < 0 || dur < 0 || dur > BAT_CTRL_RETENTION_SEC * 2)
            return -1;
        if (i > 0 && st <= in->events[i - 1].start_wall_sec)
            return -1;
    }

    if (in->has_active_session && in->active_start_wall_sec < 0)
        return -1;

    /* Validate daily summaries: UTC midnight aligned, strictly increasing,
     * each duration in [0, 86400]. */
    for (size_t i = 0; i < in->day_count; i++) {
        const struct bat_daily_summary *d = &in->days[i];
        if (d->day_utc_sec < 0 || (d->day_utc_sec % BAT_CTRL_DAY_SEC) != 0)
            return -1;
        if (i > 0 && d->day_utc_sec <= in->days[i - 1].day_utc_sec)
            return -1;
        if (d->time_on_ac_sec < 0 || d->time_on_ac_sec > BAT_CTRL_DAY_SEC)
            return -1;
        if (d->time_full_on_ac_sec < 0 || d->time_full_on_ac_sec > BAT_CTRL_DAY_SEC)
            return -1;
        if (d->hold_time_on_ac_sec < 0 || d->hold_time_on_ac_sec > BAT_CTRL_DAY_SEC)
            return -1;
    }

    struct bat_charge_history tmp;
    bat_hist_init(&tmp);

    int64_t cutoff = wall_sec - BAT_CTRL_RETENTION_SEC;
    for (size_t i = 0; i < in->event_count; i++) {
        int64_t st = bat_hist_floor_sec(in->events[i].start_wall_sec);
        int64_t dur = bat_hist_floor_sec(in->events[i].duration_sec);
        if (st + dur < cutoff)
            continue;
        hist_put_event(&tmp, st, dur);
    }

    for (size_t i = 0; i < in->day_count; i++) {
        const struct bat_daily_summary *d = &in->days[i];
        if (d->day_utc_sec < cutoff)
            continue;
        struct bat_daily_summary *dst = hist_get_or_insert_day(&tmp, d->day_utc_sec);
        if (!dst)
            continue;
        dst->time_on_ac_sec = bat_hist_floor_sec(d->time_on_ac_sec);
        dst->time_full_on_ac_sec = bat_hist_floor_sec(d->time_full_on_ac_sec);
        dst->hold_time_on_ac_sec = bat_hist_floor_sec(d->hold_time_on_ac_sec);
        tmp.duration_on_ac_sec += dst->time_on_ac_sec;
        tmp.duration_full_on_ac_sec += dst->time_full_on_ac_sec;
        tmp.hold_duration_on_ac_sec += dst->hold_time_on_ac_sec;
    }

    /* Upstream Init: drop future incomplete session; restore active session
     * only if AC is currently connected. */
    if (in->has_active_session &&
        in->active_start_wall_sec <= wall_sec &&
        power->ac_connected) {
        int64_t st = bat_hist_floor_sec(in->active_start_wall_sec);
        tmp.has_ac_connect = true;
        tmp.ac_connect_wall_sec = st;
        tmp.ac_connect_boot_sec = boot_sec;
        tmp.ac_connect_boot_offset_sec = (double)(wall_sec - st);
        hist_erase_event_at(&tmp, st);
    }

    /* Finalize via normal init path (zero-fills days through today, sets
     * full/hold start, prunes to 50/49 events and 30 days). */
    bat_hist_update(&tmp, power, adaptive_delaying_charge, wall_sec, boot_sec);
    *h = tmp;
    return 0;
}

/* ---- Internal AdaptiveChargingController helpers ---- */

static bool config_valid(const struct bat_ctrl_config *cfg)
{
    if (!cfg)
        return false;
    if (cfg->hold_percent <= 0 || cfg->hold_percent >= 100)
        return false;
    if (cfg->hold_delta_percent < 0 || cfg->hold_delta_percent >= 100)
        return false;
    if (cfg->hold_percent - cfg->hold_delta_percent < 0)
        return false;
    if (!isfinite(cfg->min_probability) ||
        cfg->min_probability < 0.0 || cfg->min_probability > 1.0)
        return false;
    if (!isfinite(cfg->max_delay_percentile) ||
        cfg->max_delay_percentile < 0.0 || cfg->max_delay_percentile > 1.0)
        return false;
    if (cfg->min_days_history < 0)
        return false;
    if (!isfinite(cfg->min_full_on_ac_ratio) ||
        cfg->min_full_on_ac_ratio < 0.0 || cfg->min_full_on_ac_ratio > 1.0)
        return false;
    if (!isfinite(cfg->recheck_interval_sec) || cfg->recheck_interval_sec <= 0.0)
        return false;
    return true;
}

static struct bat_ctrl_decision decision_from_ctrl(const struct bat_ctrl *c,
                                                   enum bat_ctrl_action action)
{
    struct bat_ctrl_decision d = {
        .action = action,
        .sustain_percent = (action == BAT_CTRL_ACTION_HOLD) ? c->sustain_percent : -1,
        .release_at = c->charge_alarm_running ? c->charge_alarm_at : -1.0,
        .target_full_at = -1.0,
        .recheck_at = c->recheck_alarm_running ? c->recheck_alarm_at : -1.0,
    };
    if (c->target_full_valid) {
        d.target_full_at = c->target_full_unbounded ? HUGE_VAL : c->target_full_boot_sec;
    }
    return d;
}

static bool ctrl_is_delaying_charge(const struct bat_ctrl *c)
{
    return c->state == BAT_CTRL_STATE_ACTIVE && c->is_sustain_set &&
           bat_ctrl_at_hold_percent(c, c->power.display_battery_pct);
}

/* Matches AdaptiveChargingController::StartRecheckAlarm. */
static void ctrl_start_recheck_alarm(struct bat_ctrl *c, double boot_sec, double delay)
{
    if (delay < 0.0)
        delay = 0.0;
    c->recheck_alarm_running = true;
    c->recheck_alarm_at = boot_sec + delay;
}

/* Matches AdaptiveChargingController::StartChargeAlarm (without slow charge). */
static void ctrl_start_charge_alarm(struct bat_ctrl *c, double boot_sec, double delay)
{
    if (delay < 0.0)
        delay = 0.0;
    c->charge_alarm_running = true;
    c->charge_alarm_at = boot_sec + delay;
    c->target_full_valid = true;
    c->target_full_unbounded = false;
    c->target_full_boot_sec = boot_sec + delay + BAT_CTRL_FINISH_CHARGING_DELAY_SEC;
}

/* Matches AdaptiveChargingController::StopAdaptiveCharging.
 * Returns true if hardware sustain was active and cleared (caller should RESUME). */
static bool ctrl_stop_adaptive(struct bat_ctrl *c, double boot_sec)
{
    if (c->state == BAT_CTRL_STATE_ACTIVE)
        c->state = BAT_CTRL_STATE_INACTIVE;

    if (!c->hold_end_valid) {
        c->hold_end_valid = true;
        c->hold_end_boot_sec = boot_sec;
    }

    c->recheck_alarm_running = false;
    c->recheck_alarm_at = -1.0;
    c->charge_alarm_running = false;
    c->charge_alarm_at = -1.0;
    c->prediction_requested = false;

    bool was_sustain = c->is_sustain_set;
    if (c->is_sustain_set) {
        c->is_sustain_set = false;
        c->sustain_percent = -1;
    }
    return was_sustain;
}

/* Matches AdaptiveChargingController::UpdateAdaptiveChargingState.
 * Evaluates support, user enable, learning heuristic, and 96%/FULL guard.
 * Sets prediction_requested = true when upstream would invoke
 * UpdateAdaptiveCharging(reason, true). */
static void ctrl_update_state(struct bat_ctrl *c,
                              enum bat_ctrl_reason reason,
                              double boot_sec)
{
    if (c->state == BAT_CTRL_STATE_SHUTDOWN)
        return;

    c->report_charge_time =
        c->power.display_battery_pct <= (double)c->cfg.hold_percent;

    int64_t hold_sec = bat_hist_hold_time_on_ac_sec(&c->history, boot_sec);
    int64_t full_sec = bat_hist_time_full_on_ac_sec(&c->history, boot_sec);
    int64_t on_ac_sec = bat_hist_time_on_ac_sec(&c->history, boot_sec);
    double ratio = 0.0;
    if (on_ac_sec > 0)
        ratio = (double)(hold_sec + full_sec) / (double)on_ac_sec;

    if (!c->cfg.supported) {
        c->state = BAT_CTRL_STATE_NOT_SUPPORTED;
    } else if (!c->cfg.enabled) {
        c->state = BAT_CTRL_STATE_USER_DISABLED;
    } else if (bat_hist_days_of_history(&c->history) < c->cfg.min_days_history ||
               ratio < c->cfg.min_full_on_ac_ratio) {
        c->state = BAT_CTRL_STATE_HEURISTIC_DISABLED;
    } else {
        c->state = BAT_CTRL_STATE_ACTIVE;
    }

    /* 96% / FULL guard: downgrade ACTIVE to INACTIVE and skip prediction. */
    if (c->power.battery_full ||
        c->power.display_battery_pct >= BAT_CTRL_MAX_START_PERCENT) {
        if (c->state == BAT_CTRL_STATE_ACTIVE)
            c->state = BAT_CTRL_STATE_INACTIVE;
        c->started = false;
        c->prediction_requested = false;
        return;
    }

    c->prediction_requested = true;
    c->prediction_reason = reason;
}

/* ---- Public AdaptiveChargingController API ---- */

void bat_ctrl_default_config(struct bat_ctrl_config *cfg)
{
    if (!cfg)
        return;
    *cfg = (struct bat_ctrl_config){
        .supported = true,
        .enabled = true,
        .hold_percent = BAT_CTRL_DEFAULT_HOLD_PERCENT,
        .hold_delta_percent = BAT_CTRL_DEFAULT_HOLD_DELTA_PERCENT,
        .min_probability = BAT_CTRL_DEFAULT_MIN_PROBABILITY,
        .max_delay_percentile = BAT_CTRL_DEFAULT_MAX_DELAY_PERCENTILE,
        .min_days_history = BAT_CTRL_DEFAULT_MIN_DAYS_HISTORY,
        .min_full_on_ac_ratio = BAT_CTRL_DEFAULT_MIN_FULL_ON_AC_RATIO,
        .recheck_interval_sec = BAT_CTRL_DEFAULT_RECHECK_INTERVAL_SEC,
    };
}

void bat_ctrl_default_context(struct bat_ctrl_context *ctx)
{
    if (!ctx)
        return;
    *ctx = (struct bat_ctrl_context){
        .local_hour = 0,
        .local_minute = 0,
        .day_of_week = 0,
        .day_of_month = 1,
        .mode = BAT_CTRL_MODE_LAPTOP,
        .screen_brightness_pct = 0,
        .reason = BAT_CTRL_REASON_PERIODIC_LOG,
    };
}

int bat_ctrl_serialize_history(const struct bat_ctrl *c,
                               int64_t *out,
                               size_t max_elems)
{
    if (!c || !out)
        return -1;

    struct bat_history_snapshot snap;
    bat_ctrl_export_history(c, &snap);

    size_t need = 5 + 2 * snap.event_count + 4 * snap.day_count;
    if (need > max_elems || need > BAT_CTRL_SERIAL_MAX_I64)
        return -1;

    size_t p = 0;
    out[p++] = BAT_CTRL_SERIAL_VERSION;
    out[p++] = snap.has_active_session ? 1 : 0;
    out[p++] = snap.active_start_wall_sec;
    out[p++] = (int64_t)snap.event_count;
    for (size_t i = 0; i < snap.event_count; i++) {
        out[p++] = snap.events[i].start_wall_sec;
        out[p++] = snap.events[i].duration_sec;
    }
    out[p++] = (int64_t)snap.day_count;
    for (size_t i = 0; i < snap.day_count; i++) {
        out[p++] = snap.days[i].day_utc_sec;
        out[p++] = snap.days[i].time_on_ac_sec;
        out[p++] = snap.days[i].time_full_on_ac_sec;
        out[p++] = snap.days[i].hold_time_on_ac_sec;
    }
    return (int)p;
}

int bat_ctrl_deserialize_history(struct bat_ctrl *c,
                                 const int64_t *in,
                                 size_t n_elems,
                                 const struct bat_ctrl_power *power,
                                 int64_t wall_sec,
                                 double boot_sec)
{
    if (!c || !in || !power || n_elems < 5)
        return -1;

    size_t p = 0;
    if (in[p++] != BAT_CTRL_SERIAL_VERSION)
        return -1;

    int64_t has_active = in[p++];
    if (has_active != 0 && has_active != 1)
        return -1;
    int64_t active_start = in[p++];
    int64_t ev_cnt = in[p++];
    if (ev_cnt < 0 || ev_cnt > BAT_CTRL_MAX_CHARGE_EVENTS)
        return -1;

    /* Check we have room for 2*ev_cnt + day_count slot without overflow. */
    size_t ev_words = (size_t)ev_cnt * 2;
    if (ev_words + 1 > n_elems - p)
        return -1;

    struct bat_history_snapshot snap;
    memset(&snap, 0, sizeof(snap));
    snap.has_active_session = (has_active == 1);
    snap.active_start_wall_sec = active_start;
    snap.event_count = (size_t)ev_cnt;
    for (size_t i = 0; i < snap.event_count; i++) {
        snap.events[i].start_wall_sec = in[p++];
        snap.events[i].duration_sec = in[p++];
    }

    int64_t day_cnt = in[p++];
    const size_t max_days = sizeof(snap.days) / sizeof(snap.days[0]);
    if (day_cnt < 0 || (size_t)day_cnt > max_days)
        return -1;

    size_t day_words = (size_t)day_cnt * 4;
    if (day_words != n_elems - p)
        return -1;

    snap.day_count = (size_t)day_cnt;
    for (size_t i = 0; i < snap.day_count; i++) {
        snap.days[i].day_utc_sec = in[p++];
        snap.days[i].time_on_ac_sec = in[p++];
        snap.days[i].time_full_on_ac_sec = in[p++];
        snap.days[i].hold_time_on_ac_sec = in[p++];
    }

    return bat_ctrl_import_history(c, &snap, power, wall_sec, boot_sec);
}

int bat_ctrl_init(struct bat_ctrl *c, const struct bat_ctrl_config *cfg)
{
    if (!c)
        return -1;

    struct bat_ctrl_config effective;
    if (cfg)
        effective = *cfg;
    else
        bat_ctrl_default_config(&effective);

    if (!config_valid(&effective))
        return -1;

    memset(c, 0, sizeof(*c));
    c->cfg = effective;
    bat_hist_init(&c->history);
    c->sustain_percent = -1;
    c->display_percent = effective.hold_percent;
    c->recheck_alarm_at = -1.0;
    c->charge_alarm_at = -1.0;

    if (!c->cfg.supported) {
        c->cfg.enabled = false;
        c->state = BAT_CTRL_STATE_NOT_SUPPORTED;
    } else if (c->cfg.enabled) {
        c->state = BAT_CTRL_STATE_INACTIVE;
    } else {
        c->state = BAT_CTRL_STATE_USER_DISABLED;
    }
    return 0;
}

int bat_ctrl_import_history(struct bat_ctrl *c,
                            const struct bat_history_snapshot *in,
                            const struct bat_ctrl_power *power,
                            int64_t wall_sec,
                            double boot_sec)
{
    if (!c || !in || !power)
        return -1;
    int rc = bat_hist_import(&c->history, in, power,
                             ctrl_is_delaying_charge(c), wall_sec, boot_sec);
    if (rc == 0)
        c->power = *power;
    return rc;
}

void bat_ctrl_export_history(const struct bat_ctrl *c,
                             struct bat_history_snapshot *out)
{
    if (!c || !out)
        return;
    bat_hist_export(&c->history, out);
}

struct bat_ctrl_decision bat_ctrl_set_config(struct bat_ctrl *c,
                                             const struct bat_ctrl_config *cfg,
                                             int64_t wall_sec,
                                             double boot_sec)
{
    if (!c || !cfg || !config_valid(cfg)) {
        return (struct bat_ctrl_decision){
            .action = BAT_CTRL_ACTION_NONE,
            .sustain_percent = -1,
            .release_at = -1.0,
            .target_full_at = -1.0,
            .recheck_at = -1.0,
        };
    }
    if (c->state == BAT_CTRL_STATE_SHUTDOWN)
        return decision_from_ctrl(c, BAT_CTRL_ACTION_NONE);

    bool new_enabled = cfg->enabled;
    bool restart = false;

    if (cfg->hold_percent != c->cfg.hold_percent ||
        cfg->hold_delta_percent != c->cfg.hold_delta_percent ||
        cfg->min_probability != c->cfg.min_probability ||
        cfg->max_delay_percentile != c->cfg.max_delay_percentile ||
        cfg->min_days_history != c->cfg.min_days_history ||
        cfg->min_full_on_ac_ratio != c->cfg.min_full_on_ac_ratio ||
        cfg->recheck_interval_sec != c->cfg.recheck_interval_sec) {
        restart = bat_ctrl_is_running(c);
    }

    c->cfg.hold_percent = cfg->hold_percent;
    c->cfg.hold_delta_percent = cfg->hold_delta_percent;
    c->cfg.min_probability = cfg->min_probability;
    c->cfg.max_delay_percentile = cfg->max_delay_percentile;
    c->cfg.min_days_history = cfg->min_days_history;
    c->cfg.min_full_on_ac_ratio = cfg->min_full_on_ac_ratio;
    c->cfg.recheck_interval_sec = cfg->recheck_interval_sec;
    c->cfg.supported = cfg->supported;

    if (!c->cfg.supported) {
        new_enabled = false;
        c->state = BAT_CTRL_STATE_NOT_SUPPORTED;
    }

    if (c->cfg.enabled != new_enabled)
        restart = true;
    c->cfg.enabled = new_enabled;

    bool released = false;
    if (c->cfg.enabled) {
        if (restart) {
            released = ctrl_stop_adaptive(c, boot_sec);
            if (c->power.ac_connected)
                ctrl_update_state(c, BAT_CTRL_REASON_PERIODIC_LOG, boot_sec);
        }
    } else {
        released = ctrl_stop_adaptive(c, boot_sec);
        ctrl_update_state(c, BAT_CTRL_REASON_PERIODIC_LOG, boot_sec);
    }

    if (c->history.initialized) {
        bat_hist_update(&c->history, &c->power, ctrl_is_delaying_charge(c),
                        wall_sec, boot_sec);
    }

    return decision_from_ctrl(c, released ? BAT_CTRL_ACTION_RESUME
                                          : BAT_CTRL_ACTION_NONE);
}

struct bat_ctrl_decision bat_ctrl_on_power(struct bat_ctrl *c,
                                           const struct bat_ctrl_power *power,
                                           int64_t wall_sec,
                                           double boot_sec)
{
    if (!c || !power) {
        return (struct bat_ctrl_decision){
            .action = BAT_CTRL_ACTION_NONE,
            .sustain_percent = -1,
            .release_at = -1.0,
            .target_full_at = -1.0,
            .recheck_at = -1.0,
        };
    }

    bool prev_ac = c->power.ac_connected;
    c->power = *power;
    bat_hist_update(&c->history, power, ctrl_is_delaying_charge(c),
                    wall_sec, boot_sec);

    enum bat_ctrl_action action = BAT_CTRL_ACTION_NONE;

    if (power->ac_connected != prev_ac) {
        if (power->ac_connected) {
            ctrl_update_state(c, BAT_CTRL_REASON_CHARGER_PLUGGED_IN, boot_sec);
        } else {
            if (ctrl_stop_adaptive(c, boot_sec))
                action = BAT_CTRL_ACTION_RESUME;

            c->target_full_valid = false;
            c->target_full_unbounded = false;
            c->target_full_boot_sec = 0.0;
            c->hold_start_valid = false;
            c->hold_start_boot_sec = 0.0;
            c->hold_end_valid = false;
            c->hold_end_boot_sec = 0.0;
            c->charge_finished_valid = false;
            c->charge_finished_boot_sec = 0.0;
            c->started = false;
            return decision_from_ctrl(c, action);
        }
    }

    if (!c->started || !power->ac_connected)
        return decision_from_ctrl(c, action);

    /* Upstream OnPowerStatusUpdate lines 1355-1382: record hold start when
     * display_battery_pct reaches hold range; record charge_finished on FULL. */
    if (bat_ctrl_at_hold_percent(c, power->display_battery_pct)) {
        if (!c->hold_start_valid) {
            c->hold_start_valid = true;
            c->hold_start_boot_sec = boot_sec;
        }
    }

    if (power->battery_full) {
        if (!c->charge_finished_valid && c->report_charge_time) {
            c->charge_finished_valid = true;
            c->charge_finished_boot_sec = boot_sec;
        }
        /* Full battery downgrades ACTIVE to INACTIVE and stops holding. */
        if (c->state == BAT_CTRL_STATE_ACTIVE) {
            if (ctrl_stop_adaptive(c, boot_sec))
                action = BAT_CTRL_ACTION_RESUME;
        }
    } else if (power->display_battery_pct >= BAT_CTRL_MAX_START_PERCENT &&
               c->state == BAT_CTRL_STATE_ACTIVE && !c->is_sustain_set) {
        /* 96% guard before sustain has engaged. */
        c->state = BAT_CTRL_STATE_INACTIVE;
        c->started = false;
        c->recheck_alarm_running = false;
        c->recheck_alarm_at = -1.0;
        c->charge_alarm_running = false;
        c->charge_alarm_at = -1.0;
        c->prediction_requested = false;
    }

    /* Refresh history hold tracking in case delaying state flipped. */
    bat_hist_update(&c->history, power, ctrl_is_delaying_charge(c),
                    wall_sec, boot_sec);

    return decision_from_ctrl(c, action);
}

struct bat_ctrl_decision bat_ctrl_tick(struct bat_ctrl *c,
                                       int64_t wall_sec,
                                       double boot_sec)
{
    if (!c) {
        return (struct bat_ctrl_decision){
            .action = BAT_CTRL_ACTION_NONE,
            .sustain_percent = -1,
            .release_at = -1.0,
            .target_full_at = -1.0,
            .recheck_at = -1.0,
        };
    }

    if (c->history.initialized) {
        bat_hist_check_time_change(&c->history, wall_sec, boot_sec);
        hist_add_zero_days(&c->history, wall_sec);
        bat_hist_prune(&c->history, wall_sec);
    }

    enum bat_ctrl_action action = BAT_CTRL_ACTION_NONE;

    /* Charge alarm takes priority: stop holding and charge to full. */
    if (c->charge_alarm_running && boot_sec >= c->charge_alarm_at) {
        if (ctrl_stop_adaptive(c, boot_sec))
            action = BAT_CTRL_ACTION_RESUME;
        if (c->history.initialized) {
            bat_hist_update(&c->history, &c->power, ctrl_is_delaying_charge(c),
                            wall_sec, boot_sec);
        }
        return decision_from_ctrl(c, action);
    }

    /* Recheck alarm fires: request a PERIODIC_LOG prediction. */
    if (c->recheck_alarm_running && boot_sec >= c->recheck_alarm_at) {
        c->recheck_alarm_running = false;
        c->recheck_alarm_at = -1.0;
        c->prediction_requested = true;
        c->prediction_reason = BAT_CTRL_REASON_PERIODIC_LOG;
    }

    return decision_from_ctrl(c, action);
}

struct bat_ctrl_decision bat_ctrl_on_prediction(struct bat_ctrl *c,
                                                bool inference_done,
                                                const double probs[BAT_ADAPTIVE_OUTPUTS],
                                                int64_t wall_sec,
                                                double boot_sec)
{
    if (!c) {
        return (struct bat_ctrl_decision){
            .action = BAT_CTRL_ACTION_NONE,
            .sustain_percent = -1,
            .release_at = -1.0,
            .target_full_at = -1.0,
            .recheck_at = -1.0,
        };
    }

    c->prediction_requested = false;

    bool probs_ok = inference_done && probs != NULL;
    if (probs_ok) {
        for (int i = 0; i < BAT_ADAPTIVE_OUTPUTS; i++) {
            if (!isfinite(probs[i])) {
                probs_ok = false;
                break;
            }
        }
    }
    if (!probs_ok) {
        bool released = ctrl_stop_adaptive(c, boot_sec);
        c->started = false;
        if (c->history.initialized) {
            bat_hist_update(&c->history, &c->power, ctrl_is_delaying_charge(c),
                            wall_sec, boot_sec);
        }
        return decision_from_ctrl(c, released ? BAT_CTRL_ACTION_RESUME
                                              : BAT_CTRL_ACTION_NONE);
    }

    /* 9-bucket CDF search (upstream lines 1139-1146):
     * sum probs[0..hour] until sum >= min_probability. */
    double prob_sum = 0.0;
    int hour;
    for (hour = 0; hour < BAT_ADAPTIVE_OUTPUTS; hour++) {
        prob_sum += probs[hour];
        if (prob_sum >= c->cfg.min_probability)
            break;
    }

    /* Compute target_delay and target_time (upstream lines 1174-1188).
     * Bucket 8 (last index) means >= 8 h -> unbounded (TimeDelta::Max). */
    bool target_unbounded = (hour >= BAT_ADAPTIVE_OUTPUTS - 1);
    double target_delay = target_unbounded ? HUGE_VAL : ((double)hour * 3600.0);
    double target_time = target_unbounded ? HUGE_VAL : (boot_sec + target_delay);

    if (c->hold_start_valid) {
        int64_t max_delay_i64 =
            bat_hist_duration_percentile_sec(&c->history, c->cfg.max_delay_percentile);
        if (max_delay_i64 != INT64_MAX) {
            double max_delay = (double)max_delay_i64;
            if (target_unbounded || (target_time - c->hold_start_boot_sec > max_delay)) {
                target_unbounded = false;
                target_time = c->hold_start_boot_sec + max_delay;
                target_delay = target_time - boot_sec;
            }
        }
    }

    /* Insufficient remaining unplug time -> resume charging (upstream lines 1192-1202). */
    if (!target_unbounded && target_delay <= BAT_CTRL_FINISH_CHARGING_DELAY_SEC) {
        bool was_holding = c->hold_start_valid;
        enum bat_ctrl_state prev_state = c->state;
        bool released = ctrl_stop_adaptive(c, boot_sec);
        c->target_full_valid = true;
        c->target_full_unbounded = false;
        if (was_holding) {
            c->target_full_boot_sec = boot_sec + BAT_CTRL_FINISH_CHARGING_DELAY_SEC;
        } else {
            c->target_full_boot_sec = boot_sec + target_delay;
        }
        if (c->history.initialized) {
            bat_hist_update(&c->history, &c->power, ctrl_is_delaying_charge(c),
                            wall_sec, boot_sec);
        }
        if (released || prev_state == BAT_CTRL_STATE_ACTIVE ||
            prev_state == BAT_CTRL_STATE_INACTIVE) {
            return decision_from_ctrl(c, BAT_CTRL_ACTION_RESUME);
        }
        return decision_from_ctrl(c, BAT_CTRL_ACTION_NONE);
    }

    c->started = true;
    ctrl_start_recheck_alarm(c, boot_sec, c->cfg.recheck_interval_sec);

    /* Schedule or preserve charge_alarm (upstream lines 1231-1257). */
    if (!target_unbounded) {
        if (c->charge_alarm_running &&
            bat_ctrl_at_hold_percent(c, c->power.display_battery_pct) &&
            c->target_full_valid && !c->target_full_unbounded &&
            target_time >= c->target_full_boot_sec) {
            /* Do not push out an already-promised full-charge time once at hold. */
            enum bat_ctrl_action act =
                (c->state == BAT_CTRL_STATE_ACTIVE && c->is_sustain_set)
                    ? BAT_CTRL_ACTION_HOLD
                    : BAT_CTRL_ACTION_NONE;
            return decision_from_ctrl(c, act);
        }
        ctrl_start_charge_alarm(c, boot_sec,
                                target_delay - BAT_CTRL_FINISH_CHARGING_DELAY_SEC);
    } else if (!(c->charge_alarm_running &&
                 bat_ctrl_at_hold_percent(c, c->power.display_battery_pct))) {
        /* Bucket 8 unbounded: no release alarm scheduled yet. */
        c->charge_alarm_running = false;
        c->charge_alarm_at = -1.0;
        c->target_full_valid = true;
        c->target_full_unbounded = true;
        c->target_full_boot_sec = HUGE_VAL;
    }

    /* Only engage hardware sustain when ACTIVE (upstream lines 1263-1280). */
    if (c->state != BAT_CTRL_STATE_ACTIVE)
        return decision_from_ctrl(c, BAT_CTRL_ACTION_NONE);

    if (!c->is_sustain_set) {
        int disp_int = (int)c->power.display_battery_pct;
        int sustain = c->cfg.hold_percent > disp_int ? c->cfg.hold_percent : disp_int;
        c->is_sustain_set = true;
        c->sustain_percent = sustain;
        c->display_percent = sustain;
    }

    if (bat_ctrl_at_hold_percent(c, c->power.display_battery_pct) &&
        !c->hold_start_valid) {
        c->hold_start_valid = true;
        c->hold_start_boot_sec = boot_sec;
    }

    if (c->history.initialized) {
        bat_hist_update(&c->history, &c->power, ctrl_is_delaying_charge(c),
                        wall_sec, boot_sec);
    }

    return decision_from_ctrl(c, BAT_CTRL_ACTION_HOLD);
}

struct bat_ctrl_decision bat_ctrl_charge_now(struct bat_ctrl *c,
                                             int64_t wall_sec,
                                             double boot_sec)
{
    if (!c) {
        return (struct bat_ctrl_decision){
            .action = BAT_CTRL_ACTION_NONE,
            .sustain_percent = -1,
            .release_at = -1.0,
            .target_full_at = -1.0,
            .recheck_at = -1.0,
        };
    }

    if (c->state == BAT_CTRL_STATE_ACTIVE)
        c->state = BAT_CTRL_STATE_USER_CANCELED;

    bool released = ctrl_stop_adaptive(c, boot_sec);
    if (c->history.initialized) {
        bat_hist_update(&c->history, &c->power, false, wall_sec, boot_sec);
    }
    return decision_from_ctrl(c, (released || c->state == BAT_CTRL_STATE_USER_CANCELED)
                                     ? BAT_CTRL_ACTION_RESUME
                                     : BAT_CTRL_ACTION_NONE);
}

struct bat_ctrl_decision bat_ctrl_on_suspend(struct bat_ctrl *c,
                                             int64_t wall_sec,
                                             double boot_sec)
{
    if (!c) {
        return (struct bat_ctrl_decision){
            .action = BAT_CTRL_ACTION_NONE,
            .sustain_percent = -1,
            .release_at = -1.0,
            .target_full_at = -1.0,
            .recheck_at = -1.0,
        };
    }

    bat_hist_enter_low_power(&c->history, wall_sec, boot_sec);

    if (bat_ctrl_is_running(c)) {
        c->prediction_requested = true;
        c->prediction_reason = BAT_CTRL_REASON_SUSPEND;
    }
    return decision_from_ctrl(c, BAT_CTRL_ACTION_NONE);
}

void bat_ctrl_on_resume(struct bat_ctrl *c, int64_t wall_sec, double boot_sec)
{
    if (!c || !c->history.initialized)
        return;
    bat_hist_check_time_change(&c->history, wall_sec, boot_sec);
    hist_add_zero_days(&c->history, wall_sec);
    bat_hist_prune(&c->history, wall_sec);
}

struct bat_ctrl_decision bat_ctrl_on_shutdown(struct bat_ctrl *c,
                                              int64_t wall_sec,
                                              double boot_sec)
{
    if (!c) {
        return (struct bat_ctrl_decision){
            .action = BAT_CTRL_ACTION_NONE,
            .sustain_percent = -1,
            .release_at = -1.0,
            .target_full_at = -1.0,
            .recheck_at = -1.0,
        };
    }

    c->cfg.enabled = false;
    c->state = BAT_CTRL_STATE_SHUTDOWN;
    bool released = ctrl_stop_adaptive(c, boot_sec);
    bat_hist_enter_low_power(&c->history, wall_sec, boot_sec);
    return decision_from_ctrl(c, released ? BAT_CTRL_ACTION_RESUME
                                          : BAT_CTRL_ACTION_NONE);
}

/* ---- Feature extraction (upstream UpdateAdaptiveCharging lines 1534-1676) ---- */

static void emit_int32(struct bat_feature *out, size_t *idx,
                       const char *name, int32_t value)
{
    out[(*idx)++] = (struct bat_feature){
        .name = name,
        .kind = BAT_FEAT_INT32,
        .value = (double)value,
    };
}

int bat_ctrl_features(const struct bat_ctrl *c,
                      const struct bat_ctrl_context *ctx,
                      int64_t wall_sec,
                      double boot_sec,
                      struct bat_feature *out,
                      size_t max)
{
    if (!c || !ctx || !out || max < BAT_CTRL_FEATURE_COUNT)
        return -1;

    size_t idx = 0;

    /* 1. Time & device scalars (upstream lines 1534-1573). */
    int minutes = 60 * ctx->local_hour + ctx->local_minute;
    minutes -= minutes % BAT_CTRL_TIME_BUCKET_MIN;
    emit_int32(out, &idx, "TimeOfTheDay", (int32_t)minutes);
    emit_int32(out, &idx, "DayOfWeek", (int32_t)ctx->day_of_week);
    emit_int32(out, &idx, "DayOfMonth", (int32_t)ctx->day_of_month);
    emit_int32(out, &idx, "DeviceMode", (int32_t)ctx->mode);
    emit_int32(out, &idx, "BatteryPercentage", (int32_t)c->power.battery_pct);
    emit_int32(out, &idx, "IsCharging", c->power.ac_connected ? 1 : 0);
    emit_int32(out, &idx, "ScreenBrightnessPercent", (int32_t)ctx->screen_brightness_pct);
    emit_int32(out, &idx, "Reason", (int32_t)ctx->reason);

    /* Apply the clock-step rule on a read-only local copy so features stay
     * accurate even if wall_sec stepped since the last power/tick call. */
    struct bat_charge_history hcopy = c->history;
    bat_hist_check_time_change(&hcopy, wall_sec, boot_sec);

    /* 2. RatioTimeFullCharge (upstream lines 1576-1584):
     *    int32(10 * (time_full_on_ac + hold_time_on_ac) / time_on_ac), 0 if on_ac == 0. */
    int64_t time_on_ac = bat_hist_time_on_ac_sec(&hcopy, boot_sec);
    int32_t ratio_feat = 0;
    if (time_on_ac > 0) {
        int64_t time_full = bat_hist_time_full_on_ac_sec(&hcopy, boot_sec);
        int64_t hold_time = bat_hist_hold_time_on_ac_sec(&hcopy, boot_sec);
        ratio_feat = (int32_t)((10 * (time_full + hold_time)) / time_on_ac);
    }
    emit_int32(out, &idx, "RatioTimeFullCharge", ratio_feat);

    /* 3. Charge events (upstream CopyToProtocolBuffer + lines 1588-1623).
     * CopyToProtocolBuffer lists completed events ascending, then appends the
     * active (incomplete, no duration) AC session if present. UpdateAdaptiveCharging
     * iterates in reverse order: event 0 is the active session (if any), followed
     * by completed events newest-first. */
    struct {
        bool has_start;
        bool has_dur;
        int64_t start_wall_sec;
        int64_t duration_sec;
    } evs[BAT_CTRL_MAX_CHARGE_EVENTS + 1];
    size_t ev_total = 0;

    for (size_t i = 0; i < hcopy.event_count && ev_total < BAT_CTRL_MAX_CHARGE_EVENTS; i++) {
        evs[ev_total].has_start = true;
        evs[ev_total].has_dur = true;
        evs[ev_total].start_wall_sec = hcopy.events[i].start_wall_sec;
        evs[ev_total].duration_sec = hcopy.events[i].duration_sec;
        ev_total++;
    }
    if (hcopy.has_ac_connect) {
        evs[ev_total].has_start = true;
        evs[ev_total].has_dur = false;
        evs[ev_total].start_wall_sec = hcopy.ac_connect_wall_sec;
        evs[ev_total].duration_sec = 0;
        ev_total++;
    }

    emit_int32(out, &idx, "ChargeEventHistorySize", (int32_t)ev_total);

    int slot = 0;
    for (size_t rev = ev_total; rev > 0 && slot < BAT_CTRL_MAX_CHARGE_EVENTS; rev--, slot++) {
        size_t j = rev - 1;
        int32_t dur_min = evs[j].has_dur ? (int32_t)(evs[j].duration_sec / 60) : -1;
        int32_t start_min = evs[j].has_start
                                ? (int32_t)((wall_sec - evs[j].start_wall_sec) / 60)
                                : -1;
        emit_int32(out, &idx, k_event_dur_names[slot], dur_min);
        emit_int32(out, &idx, k_event_start_names[slot], start_min);
    }
    for (; slot < BAT_CTRL_MAX_CHARGE_EVENTS; slot++) {
        emit_int32(out, &idx, k_event_dur_names[slot], -1);
        emit_int32(out, &idx, k_event_start_names[slot], -1);
    }

    /* 4. Daily summaries (upstream CopyToProtocolBuffer + lines 1625-1676).
     * Build a read-only projected view of daily summaries including zero-filled
     * missing days up to today's UTC midnight and any active session overlap. */
    struct bat_daily_summary proj[BAT_CTRL_MAX_RETENTION_DAYS + 2];
    size_t proj_cap = sizeof(proj) / sizeof(proj[0]);
    size_t proj_count = 0;

    for (size_t i = 0; i < hcopy.day_count && proj_count < proj_cap; i++)
        proj[proj_count++] = hcopy.days[i];

    int64_t today = bat_hist_utc_midnight(wall_sec);
    if (hcopy.initialized) {
        int64_t min_day = bat_hist_utc_midnight(wall_sec - BAT_CTRL_RETENTION_SEC);
        int64_t d_start = (proj_count == 0)
                              ? today
                              : (proj[proj_count - 1].day_utc_sec + BAT_CTRL_DAY_SEC);
        if (d_start < min_day)
            d_start = min_day;
        for (; d_start <= today; d_start += BAT_CTRL_DAY_SEC) {
            if (proj_count >= proj_cap) {
                memmove(&proj[0], &proj[1], (proj_count - 1) * sizeof(proj[0]));
                proj_count--;
            }
            proj[proj_count++] = (struct bat_daily_summary){
                .day_utc_sec = d_start,
                .time_on_ac_sec = 0,
                .time_full_on_ac_sec = 0,
                .hold_time_on_ac_sec = 0,
            };
        }
    }

    for (size_t i = 0; i < proj_count; i++) {
        int64_t day_start = proj[i].day_utc_sec;
        int64_t on_ac = proj[i].time_on_ac_sec +
                        hist_duration_for_day(hcopy.has_ac_connect,
                                              hcopy.ac_connect_wall_sec,
                                              day_start, wall_sec);
        int64_t full = proj[i].time_full_on_ac_sec +
                       hist_duration_for_day(hcopy.has_full_charge,
                                             hcopy.full_charge_wall_sec,
                                             day_start, wall_sec);
        int64_t hold = proj[i].hold_time_on_ac_sec +
                       hist_duration_for_day(hcopy.has_hold_charge,
                                             hcopy.hold_charge_wall_sec,
                                             day_start, wall_sec);
        proj[i].time_on_ac_sec = bat_hist_floor_sec(on_ac);
        proj[i].time_full_on_ac_sec = bat_hist_floor_sec(full);
        proj[i].hold_time_on_ac_sec = bat_hist_floor_sec(hold);
    }

    emit_int32(out, &idx, "DailySummarySize", (int32_t)proj_count);

    /* Walk proj from newest to oldest, matching date = today - day * 1d. */
    size_t rpos = proj_count;
    for (int day = 0; day < BAT_CTRL_MAX_RETENTION_DAYS; day++) {
        int64_t date = today - (int64_t)day * BAT_CTRL_DAY_SEC;
        while (rpos > 0 && proj[rpos - 1].day_utc_sec > date)
            rpos--;

        if (rpos == 0 || proj[rpos - 1].day_utc_sec < date) {
            emit_int32(out, &idx, k_day_hold_names[day], -1);
            emit_int32(out, &idx, k_day_full_names[day], -1);
            emit_int32(out, &idx, k_day_on_ac_names[day], -1);
            continue;
        }

        const struct bat_daily_summary *ds = &proj[rpos - 1];
        emit_int32(out, &idx, k_day_hold_names[day],
                   (int32_t)(ds->hold_time_on_ac_sec / 60));
        emit_int32(out, &idx, k_day_full_names[day],
                   (int32_t)(ds->time_full_on_ac_sec / 60));
        emit_int32(out, &idx, k_day_on_ac_names[day],
                   (int32_t)(ds->time_on_ac_sec / 60));
        rpos--;
    }

    return (int)idx;
}

bool bat_ctrl_at_hold_percent(const struct bat_ctrl *c, double display_battery_pct)
{
    if (!c)
        return false;
    return display_battery_pct >=
           (double)(c->cfg.hold_percent - c->cfg.hold_delta_percent - 1);
}

bool bat_ctrl_is_running(const struct bat_ctrl *c)
{
    return c && c->recheck_alarm_running;
}

const char *bat_ctrl_state_name(enum bat_ctrl_state state)
{
    switch (state) {
    case BAT_CTRL_STATE_ACTIVE:             return "ACTIVE";
    case BAT_CTRL_STATE_INACTIVE:           return "INACTIVE";
    case BAT_CTRL_STATE_HEURISTIC_DISABLED: return "HEURISTIC_DISABLED";
    case BAT_CTRL_STATE_USER_CANCELED:      return "USER_CANCELED";
    case BAT_CTRL_STATE_USER_DISABLED:      return "USER_DISABLED";
    case BAT_CTRL_STATE_SHUTDOWN:           return "SHUTDOWN";
    case BAT_CTRL_STATE_NOT_SUPPORTED:      return "NOT_SUPPORTED";
    }
    return "UNKNOWN";
}
