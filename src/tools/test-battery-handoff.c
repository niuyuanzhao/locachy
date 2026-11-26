/*
 * Removable-battery pack identity & handoff regression test:
 *   1. Per-pack identity (.tsv + .meta) separate from slot name ("BAT0")
 *   2. Hot-remove on AC -> 'R' break sample + REMOVE event + offline spare retention
 *   3. Hot-insert of another pack -> INSERT event + automatic EC-safe threshold apply
 *   4. Cold-swap across hibernate/restart -> SWAP handoff event + outgoing pack marked 'R'
 *   5. Dual-battery active discharge handoff (BAT1 -> BAT0) -> DISCHARGE event
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

static void test_adaptive(int64_t t0)
{
    const char *start = "sys/BAT0/charge_control_start_threshold";
    const char *end = "sys/BAT0/charge_control_end_threshold";
    const char *ac = "sys/AC/online";
    char expiry[96], end_path[1280];
    snprintf(end_path, sizeof end_path, "%s/%s", root, end);
    put_txt(ac, "1\n");
    put_txt("sys/BAT0/status", "Not charging\n");
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
    put_txt("sys/BAT1/present", "0\n");
    assert(record_snapshot(0) == 1);
    charge_status_for("BAT0", "learning", -1, 87, 0);
    assert(get_int(start) == 63 && get_int(end) == 87);

    /* Repeated snapshots must not write unchanged threshold values. */
    struct timespec stamps[2] = {{ .tv_sec = 100 }, { .tv_sec = 100 }};
    assert(utimensat(AT_FDCWD, end_path, stamps, 0) == 0);
    set_now(t0 + 60);
    assert(record_snapshot(0) == 1);
    struct stat st;
    assert(stat(end_path, &st) == 0 && st.st_mtim.tv_sec == 100);

    /* Supply verified history at the real idle-AC cadence. */
    for (int64_t ts = t0 + 360; ts <= t0 + 3 * BAT_ADAPTIVE_DAY + 60; ts += 300) {
        struct bat_sample sample = { .ts = ts, .decipct = 800, .state = 'N', .ac = 1, .ac_verified = 1 };
        assert(bat_append_sample("Panasonic_CF-VZSU1QJS_9001", &sample) == 0);
    }
    t0 += 3 * BAT_ADAPTIVE_DAY;
    set_now(t0 + 60);
    assert(record_snapshot(1) == 1);
    charge_status_for("BAT0", "mains", 80, 80, 0);
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
    set_now(t0 + 120);
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
    puts("PASS adaptive battery charge handoff: unsupported/no-write, readback, per-pack restore, "
         "one-time full expiry, restart persistence, and status");
    return 0;
}
