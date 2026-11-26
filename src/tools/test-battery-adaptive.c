/* Fixed-time tests for the pure, stateless adaptive charge policy. */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <assert.h>
#include <stdint.h>
#include <stdio.h>

#include "../progs/battery-adaptive.h"

#define TEST_BASE 1700000000LL
#define DEPARTURE(day) (TEST_BASE + (day) * BAT_ADAPTIVE_DAY + 10 * 3600)
#define MAX_SAMPLES 4096

static struct bat_sample samples[MAX_SAMPLES];
static size_t nsamples;

static void reset_samples(void)
{
    nsamples = 0;
}

static void add_sample(int64_t ts, int dpct, char state, int ac)
{
    assert(nsamples < MAX_SAMPLES);
    assert(ts >= 946684800LL && dpct >= 0 && dpct <= 1000);
    samples[nsamples++] = (struct bat_sample){
        .ts = ts, .decipct = dpct, .state = state, .ac = ac, .ac_verified = 1,
        .power_mw = 0, .energy_mwh = 0, .voltage_mv = 0,
    };
}

static struct bat_adaptive_decision decide(void)
{
    assert(nsamples > 0);
    return bat_adaptive_decide(samples, nsamples, samples[nsamples - 1].ts);
}

static void expect_state(enum bat_adaptive_state state, int start, int end)
{
    struct bat_adaptive_decision d = decide();
    assert(d.state == state);
    assert(d.start_pct == start && d.end_pct == end);
    assert(d.release_ts == 0);
}

static void fill_ac(int64_t from, int64_t to, int64_t step, char state, int dpct)
{
    for (int64_t ts = from; ts <= to; ts += step)
        add_sample(ts, dpct, state, 1);
}

/* A normal 5-minute recorder cadence must not be called a gap. */
static void test_warmup_and_mains(void)
{
    reset_samples();
    fill_ac(TEST_BASE, TEST_BASE + 6 * 3600, 300, 'N', 800);
    expect_state(BAT_ADAPTIVE_LEARNING, -1, -1);

    reset_samples();
    fill_ac(TEST_BASE, TEST_BASE + 3 * BAT_ADAPTIVE_DAY, 300, 'N', 800);
    expect_state(BAT_ADAPTIVE_MAINS, 75, 80);

    /* A recent sparse series spanning days is not equivalent to coverage. */
    reset_samples();
    fill_ac(TEST_BASE, TEST_BASE + 6 * BAT_ADAPTIVE_DAY, 3600, 'N', 800);
    expect_state(BAT_ADAPTIVE_LEARNING, -1, -1);

    reset_samples();
    fill_ac(TEST_BASE, TEST_BASE + 3 * BAT_ADAPTIVE_DAY, 330, 'N', 800);
    /* Duplicate live/disk samples from the same second add no coverage and
     * must not split a continuous run either. */
    add_sample(samples[nsamples - 1].ts, 800, 'N', 1);
    expect_state(BAT_ADAPTIVE_MAINS, 75, 80);

    /* Old rows and AC inferred from Full/Charging remain graphable only. */
    for (size_t i = 0; i + 1 < nsamples; i++) samples[i].ac_verified = 0;
    expect_state(BAT_ADAPTIVE_LEARNING, -1, -1);

    struct bat_sample row;
    assert(bat_parse_sample_line("1700000000\t800\t0\t0\t0\tN\t1\n", &row) == 0);
    assert(row.ac == 1 && !row.ac_verified);
    assert(bat_parse_sample_line("1700000000\t800\t0\t0\t0\tN\t0\t1\n", &row) == 0);
    assert(row.ac == 0 && row.ac_verified);
}

static void test_mobility(void)
{
    reset_samples();
    fill_ac(TEST_BASE, TEST_BASE + 3 * BAT_ADAPTIVE_DAY, 300, 'N', 800);
    for (int64_t ts = TEST_BASE + 3 * BAT_ADAPTIVE_DAY + 300;
         ts <= TEST_BASE + 3 * BAT_ADAPTIVE_DAY + 2 * 3600; ts += 300)
        add_sample(ts, 700, 'D', 0);
    expect_state(BAT_ADAPTIVE_MOBILITY, -1, -1);
}

/* Daily events are deliberately generated from fixed epochs. The pack spends
 * two hours off after a long AC run, then returns to AC. Two distinct charge
 * traces reach 100% before a departure: 1 h on day one and 2 h on day two. */
static void build_repeated_unplug(int skip_gap, int charge_traces)
{
    reset_samples();
    int64_t end = DEPARTURE(3) + 4 * 3600;
    for (int64_t ts = TEST_BASE; ts <= end; ts += 300) {
        if (skip_gap && ts == DEPARTURE(2) - 300) continue;
        int64_t rel = ts - TEST_BASE;
        int day = (int)(rel / BAT_ADAPTIVE_DAY);
        int64_t tod = rel % BAT_ADAPTIVE_DAY;
        int charge_seconds = day == 1 ? 3600 : 2 * 3600;
        int64_t charge_elapsed = tod - (9 * 3600 - charge_seconds);
        if (day >= 1 && day <= 3 && tod >= 10 * 3600 && tod < 12 * 3600) {
            add_sample(ts, 950 - (int)((tod - 10 * 3600) * 250 / 7200), 'D', 0);
        } else if (day >= 1 && day <= charge_traces && charge_elapsed >= 0 && charge_elapsed <= charge_seconds) {
            int dpct = 750 + (int)(charge_elapsed * 250 / charge_seconds);
            add_sample(ts, dpct, dpct == 1000 ? 'F' : 'C', 1);
        } else {
            add_sample(ts, 800, 'N', 1);
        }
    }
}

static void test_repeated_unplug(void)
{
    build_repeated_unplug(0, 2);
    struct bat_adaptive_decision hold = decide();
    assert(hold.state == BAT_ADAPTIVE_HOLD);
    assert(hold.start_pct == 75 && hold.end_pct == 80);
    int64_t release = DEPARTURE(4) - 3 * 3600;
    assert(hold.release_ts == release);  /* Slowest measured charge plus 1 h. */

    fill_ac(samples[nsamples - 1].ts + 300, release - 300, 300, 'N', 800);
    add_sample(release - 1, 800, 'N', 1);
    assert(decide().state == BAT_ADAPTIVE_HOLD);
    add_sample(release, 800, 'N', 1);
    struct bat_adaptive_decision top_up = decide();
    assert(top_up.state == BAT_ADAPTIVE_TOP_UP);
    assert(top_up.start_pct == 99 && top_up.end_pct == 100);
    assert(top_up.release_ts == release);

    fill_ac(release + 300, DEPARTURE(4) + 2 * 3600, 300, 'F', 1000);
    expect_state(BAT_ADAPTIVE_MOBILITY, -1, -1); /* Missed departure. */

    /* One trace is insufficient; give the pack more time before departure. */
    build_repeated_unplug(0, 1);
    hold = decide();
    assert(hold.state == BAT_ADAPTIVE_HOLD);
    assert(hold.release_ts == DEPARTURE(4) - 6 * 3600);

    /* A missed observation cannot be guessed over to manufacture the third
     * recurring event. */
    build_repeated_unplug(1, 2);
    expect_state(BAT_ADAPTIVE_MOBILITY, -1, -1);

    /* Reinserted pack: don't reuse the previous stint to infer a schedule. */
    build_repeated_unplug(0, 2);
    int64_t now = samples[nsamples - 1].ts;
    add_sample(now + 300, 800, 'R', 1);
    add_sample(now + 600, 800, 'N', 1);
    expect_state(BAT_ADAPTIVE_LEARNING, -1, -1);
}

static void test_time_rollback(void)
{
    reset_samples();
    fill_ac(TEST_BASE, TEST_BASE + 3 * BAT_ADAPTIVE_DAY, 300, 'N', 800);
    /* Clock steps backward three days; only the short post-rollback suffix is
     * trusted, rather than stitching old observations across the rollback. */
    int64_t rollback = TEST_BASE + BAT_ADAPTIVE_DAY;
    fill_ac(rollback, rollback + 6 * 3600, 300, 'N', 800);
    expect_state(BAT_ADAPTIVE_LEARNING, -1, -1);

    reset_samples();
    add_sample(TEST_BASE, 800, 'N', 1);
    add_sample(TEST_BASE + 300, 800, 'N', 1);
    add_sample(TEST_BASE + 601, 800, 'N', 1); /* not a false 5-minute gap */
    fill_ac(TEST_BASE + 901, TEST_BASE + 3 * BAT_ADAPTIVE_DAY, 300, 'N', 800);
    expect_state(BAT_ADAPTIVE_MAINS, 75, 80);
}

int main(void)
{
    test_warmup_and_mains();
    test_mobility();
    test_repeated_unplug();
    test_time_rollback();
    puts("PASS adaptive battery policy: warm-up, mains, mobility, recurring unplug, gap, and clock rollback");
    return 0;
}
