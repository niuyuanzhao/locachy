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
    nslots = 0;
    any_prior_pack = 0;
    active_dis_slot[0] = '\0';
    active_dis_pack[0] = '\0';
    active_dis_dpct = 0;
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
    return 0;
}
