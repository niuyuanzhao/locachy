/* Deterministic charge policy over one physical pack's observations. */
#ifndef BATTERY_ADAPTIVE_H
#define BATTERY_ADAPTIVE_H

#include "battery-history.h"

#define BAT_ADAPTIVE_DAY          86400
#define BAT_ADAPTIVE_WINDOW       (7 * BAT_ADAPTIVE_DAY)
#define BAT_ADAPTIVE_MAX_GAP      360    /* Recorder: 1 min active, 5 min idle. */
#define BAT_ADAPTIVE_MIN_COVERAGE (24 * 3600)
#define BAT_ADAPTIVE_DAY_COVERAGE (6 * 3600)
#define BAT_ADAPTIVE_MIN_AC_RUN   (2 * 3600)
#define BAT_ADAPTIVE_MIN_OFF_RUN  (30 * 60)
#define BAT_ADAPTIVE_PHASE_RANGE  3600
#define BAT_ADAPTIVE_DEFAULT_LEAD (6 * 3600)

enum bat_adaptive_state {
    BAT_ADAPTIVE_LEARNING = 0,
    BAT_ADAPTIVE_MAINS,
    BAT_ADAPTIVE_MOBILITY,
    BAT_ADAPTIVE_HOLD,
    BAT_ADAPTIVE_TOP_UP,
};

struct bat_adaptive_decision {
    enum bat_adaptive_state state;
    int start_pct, end_pct;
    int64_t release_ts;  /* Only HOLD/TOP_UP; TOP_UP's release may be past. */
};

static inline int bat_adaptive_observed(const struct bat_sample *s)
{
    return s->ac_verified && (s->ac == 0 || s->ac == 1) && s->decipct >= 0 && s->decipct <= 1000 &&
           (s->state == 'C' || s->state == 'D' || s->state == 'F' || s->state == 'N');
}

/* Only estimate from complete, uninterrupted climbs through the top of the
 * charge curve, where taper matters. The slower observed session wins. Anchor
 * the estimator to the last departure so it cannot shorten today's lead as
 * samples age out or while today's top-up is itself still in progress. */
static inline int64_t bat_adaptive_charge_lead(const struct bat_sample *samples,
                                               size_t count, int64_t departure)
{
    int64_t cutoff = departure - 6 * BAT_ADAPTIVE_DAY;
    const struct bat_sample *prev = NULL, *start = NULL;
    int completed = 0, finished = 0;
    int64_t slowest = 0;
    for (size_t i = 0; i < count; i++) {
        const struct bat_sample *s = &samples[i];
        if (s->ts < cutoff) {
            prev = NULL;
            start = NULL;
            finished = 0;
            continue;
        }
        if (s->ts > departure) break;
        if (!bat_adaptive_observed(s) || !s->ac || (s->state != 'C' && s->state != 'F')) {
            start = prev = NULL;
            finished = 0;
            continue;
        }
        if (!prev || s->ts - prev->ts > BAT_ADAPTIVE_MAX_GAP) {
            start = NULL;
            finished = 0;
        } else if (s->decipct + 10 < prev->decipct) {
            start = NULL;
        }
        if (!finished && !start && s->state == 'C' && s->decipct >= 750 && s->decipct <= 810)
            start = s;
        if (start && s->decipct >= 990) {
            int64_t elapsed = s->ts - start->ts;
            if (elapsed >= 30 * 60) {
                /* Budget the whole 75–100% band and an extra hour. */
                int gain = s->decipct - start->decipct;
                int64_t lead = (elapsed * 250 + gain - 1) / gain + 3600;
                if (lead > slowest) slowest = lead;
                completed++;
            }
            start = NULL;
            finished = 1;
        }
        if (s->state == 'F') {
            start = NULL;
            finished = 1;
        }
        prev = s;
    }
    return completed >= 2 ? (slowest > 7200 ? slowest : 7200) : BAT_ADAPTIVE_DEFAULT_LEAD;
}

/* Input is the pack's append order, with its live observation at now last.
 * Do not sort away backwards time or combine records from different packs.
 * This function neither reads a clock nor remembers a previous decision. */
static inline struct bat_adaptive_decision bat_adaptive_decide(const struct bat_sample *samples,
                                                                size_t count, int64_t now)
{
    const struct bat_adaptive_decision learning = { BAT_ADAPTIVE_LEARNING, -1, -1, 0 };
    const struct bat_adaptive_decision mobility = { BAT_ADAPTIVE_MOBILITY, -1, -1, 0 };
    if (!samples || !count || now <= 0 || now > INT64_MAX - 3 * BAT_ADAPTIVE_DAY ||
        samples[count - 1].ts != now || !bat_adaptive_observed(&samples[count - 1]))
        return learning;

    int64_t cutoff = now > BAT_ADAPTIVE_WINDOW ? now - BAT_ADAPTIVE_WINDOW : 0;
    size_t first = 0;
    for (size_t i = 0; i < count; i++) {
        const struct bat_sample *s = &samples[i];
        if (s->ts <= 0 || s->ts > now || s->state == 'R' || s->state == 'I')
            first = i + 1;
        else if (i > 0 && s->ts < samples[i - 1].ts)
            first = i;  /* Regain confidence from the new clock's own observations. */
    }

    /* Each interval must have positive, bounded duration and agreeing AC
     * endpoints. Even matching endpoints do not explain a long gap. */
    int coverage[8] = {0};  /* Seven elapsed days can touch eight UTC dates. */
    int observed = 0, off = 0, sustained_off = 0;
    int qualified = 0, counted = 0;
    int64_t run_start = 0, departures[3] = {0};
    const struct bat_sample *prev = NULL;
    for (size_t i = first; i < count; i++) {
        const struct bat_sample *s = &samples[i];
        if (s->ts < cutoff) continue;
        if (!bat_adaptive_observed(s)) {
            prev = NULL;
            continue;
        }
        int64_t elapsed = prev ? s->ts - prev->ts : 0;
        if (!prev || elapsed > BAT_ADAPTIVE_MAX_GAP || s->ac != prev->ac) {
            /* Prior complete episodes remain evidence, but a gap or conflicting
             * same-second samples cannot supply the start of a new episode. */
            qualified = prev && elapsed > 0 && elapsed <= BAT_ADAPTIVE_MAX_GAP &&
                        !s->ac && prev->ac && prev->ts - run_start >= BAT_ADAPTIVE_MIN_AC_RUN;
            run_start = s->ts;
            counted = 0;
        } else if (elapsed > 0) {
            int day = (int)(prev->ts / BAT_ADAPTIVE_DAY - cutoff / BAT_ADAPTIVE_DAY);
            int left = BAT_ADAPTIVE_DAY - (int)(prev->ts % BAT_ADAPTIVE_DAY);
            int seconds = (int)elapsed;
            if (seconds <= left) {
                coverage[day] += seconds;
            } else {
                coverage[day] += left;
                coverage[day + 1] += seconds - left;
            }
            observed += seconds;
            if (!s->ac) {
                off += seconds;
                if (!counted && s->ts - run_start >= BAT_ADAPTIVE_MIN_OFF_RUN) {
                    counted = 1;
                    sustained_off = 1;
                    if (qualified) {
                        departures[0] = departures[1];
                        departures[1] = departures[2];
                        departures[2] = run_start;
                    } else {
                        /* Significant use with an unobserved or untimely start
                         * is not the learned pattern. Require three new events. */
                        departures[0] = departures[1] = departures[2] = 0;
                    }
                }
            }
        }
        prev = s;
    }
    int covered_days = 0;
    for (size_t i = 0; i < sizeof coverage / sizeof coverage[0]; i++)
        if (coverage[i] >= BAT_ADAPTIVE_DAY_COVERAGE) covered_days++;
    if (observed < BAT_ADAPTIVE_MIN_COVERAGE || covered_days < 3) return learning;
    if (!prev->ac || prev->state == 'D') return mobility;

    if (departures[0]) {
        /* Project the last three departures onto the same next day, including
         * across midnight. Use the earliest projection, not their average. */
        int64_t earliest = departures[2] + BAT_ADAPTIVE_DAY, latest = earliest;
        for (int i = 0; i < 2; i++) {
            int64_t projected = departures[i] + (3 - i) * BAT_ADAPTIVE_DAY;
            if (projected < earliest) earliest = projected;
            if (projected > latest) latest = projected;
        }
        if (latest - earliest <= BAT_ADAPTIVE_PHASE_RANGE &&
            now <= latest + BAT_ADAPTIVE_PHASE_RANGE) {
            int64_t release = earliest - bat_adaptive_charge_lead(samples + first, count - first,
                                                                  departures[2]);
            if (now >= release)
                return (struct bat_adaptive_decision){ BAT_ADAPTIVE_TOP_UP, 99, 100, release };
            if (now - run_start >= BAT_ADAPTIVE_MIN_AC_RUN)
                return (struct bat_adaptive_decision){ BAT_ADAPTIVE_HOLD, 75, 80, release };
            return mobility;
        }
    }
    if (!sustained_off && off * 20 <= observed && now - run_start >= BAT_ADAPTIVE_MIN_AC_RUN)
        return (struct bat_adaptive_decision){ BAT_ADAPTIVE_MAINS, 75, 80, 0 };
    return mobility;
}

#endif
