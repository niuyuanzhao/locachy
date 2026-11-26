/*
 * Power & Battery category and interactive Energy / Battery Curves page
 * (included by settings.c).
 *
 * Follows KDE Info Center's Energy Information (kcm_energyinfo) extended for
 * removable and dual-battery laptops: tracks packs by physical identity
 * (retaining swapped-out spare packs), distinguishes uninserted ('R') breaks
 * from sleep/off gaps, plots slot swap & discharge handoff markers, and
 * manages EC charge start/stop thresholds.
 */

#include <sys/inotify.h>

#include "battery-history.h"
#include "battery-model-pins.h"

#define POWER_CARD_BG   0x162230u
#define POWER_GRID_COL  0x24364Au
#define POWER_GAP_COL   0x5D6D7Eu
#define POWER_HOFF_COL  0xC39BD3u
#define POWER_CHG_COL   0x58D68Du
#define POWER_CHG_FILL  0x1A3830u
#define POWER_DIS_COL   0x5DADE2u
#define POWER_DIS_FILL  0x182E44u
#define POWER_LOW_COL   0xEC7063u
#define POWER_LOW_FILL  0x3A1E24u
#define POWER_IDLE_COL  0x85C1E9u
#define POWER_CUR_COL   0xF7DC6Fu
#define POWER_GOOD_COL  0x7DCEA0u
#define POWER_WARN_COL  0xF8C471u

static struct bat_device power_devs[BAT_MAX_DEVICES];
static struct bat_ac_info power_ac;
static int npower_devs;
static int power_loaded;
static double power_loaded_at = -100.0;
static int power_handoff_loaded;
static char power_last_handoff[128];
static struct bat_charge_status power_charge;
static int power_charge_known, power_full_pending;
static int bat_model_job;
static char bat_model_what[16];
static int bat_model_remove_asked;
int rc_bat_start = -1, rc_bat_end = -1;

static void power_format_handoff(const struct bat_handoff_event *ev, char *out, size_t size);

static int power_thresh_bat_index(void)
{
    for (int i = 0; i < npower_devs; i++) {
        if (!power_devs[i].is_peripheral && !power_devs[i].is_offline_pack &&
            (power_devs[i].has_start_thresh || power_devs[i].has_end_thresh)) {
            return i;
        }
    }
    return -1;
}

static void power_refresh_charge_status(void)
{
    power_charge_known = bat_read_charge_status(&power_charge) == 0;
    if ((power_charge_known && power_charge.full_until == rc_bat_full_until) ||
        power_ac.system != BAT_AC_ONLINE || rc_bat_full_until <= bat_now_ts()) power_full_pending = 0;
}

static int power_charge_slot_for(const struct bat_device *b)
{
    if (!power_charge_known) return -1;
    for (int i = 0; i < power_charge.nslots; i++) {
        if (!strcmp(power_charge.slots[i].slot, b->name) &&
            !strcmp(power_charge.slots[i].pack_id, b->pack_id)) return i;
    }
    return -1;
}

static const char *power_charge_state_label(const char *state)
{
    if (!strcmp(state, "manual")) return _("Manual charging");
    if (!strcmp(state, "on-battery")) return _("Running on battery");
    if (!strcmp(state, "learning")) return _("Learning usage");
    if (!strcmp(state, "mains")) return _("Mostly plugged in");
    if (!strcmp(state, "mobility")) return _("Ready for portable use");
    if (!strcmp(state, "hold")) return _("Holding charge");
    if (!strcmp(state, "top-up")) return _("Topping up before unplugging");
    if (!strcmp(state, "full")) return _("Full charge override active");
    if (!strcmp(state, "model-missing")) return _("Model not downloaded");
    if (!strcmp(state, "model-invalid")) return _("Model verification failed");
    if (!strcmp(state, "inactive")) return _("Adaptive standby");
    if (!strcmp(state, "unsupported")) return _("Charging control unsupported");
    if (!strcmp(state, "error")) return _("Could not apply charging limits");
    return _("Charging status unavailable");
}

static void power_refresh_devices(void)
{
    npower_devs = bat_scan(power_devs, BAT_MAX_DEVICES, &power_ac);
    power_refresh_charge_status();
    power_loaded = 1;
    power_loaded_at = settings_mono_sec();
    power_handoff_loaded = 0;
    power_last_handoff[0] = '\0';
    entries_dirty = 1;
    full_redraw = 1;
}

static int power_charge_watch_open(void)
{
    int fd = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
    if (fd >= 0 && inotify_add_watch(fd, bat_history_dir(), IN_MOVED_TO | IN_DELETE) < 0) {
        close(fd);
        return -1;
    }
    return fd;
}

static int power_charge_watch_changed(int fd)
{
    union { struct inotify_event aligned; char buf[4096]; } events;
    int changed = 0;
    ssize_t n;
    while ((n = read(fd, events.buf, sizeof events.buf)) > 0) {
        for (size_t i = 0; i < (size_t)n;) {
            const struct inotify_event *ev = (const void *)(events.buf + i);
            if ((ev->len && !strcmp(ev->name, "charge-status")) || (ev->mask & (IN_Q_OVERFLOW | IN_IGNORED)))
                changed = 1;
            i += sizeof *ev + ev->len;
        }
    }
    return changed;
}

static void power_refresh_devices_if_stale(void)
{
    if (!power_loaded || settings_mono_sec() - power_loaded_at >= 5.0) {
        power_refresh_devices();
    }
}

/* Load the most recent battery handoff lazily when the Power page is built,
 * never during settings --apply at boot. */
static void power_ensure_last_handoff(void)
{
    if (power_handoff_loaded) return;
    power_handoff_loaded = 1;
    power_last_handoff[0] = '\0';
    struct bat_handoff_event evs[4];
    int nev = bat_load_handoffs(0, evs, 4);
    if (nev > 0) {
        char htxt[256];
        power_format_handoff(&evs[nev - 1], htxt, sizeof htxt);
        snprintf(power_last_handoff, sizeof power_last_handoff, _("Last handoff: %.104s"), htxt);
    }
}

static int power_thresh_count(void)
{
    if (!power_loaded) power_refresh_devices();
    int bi = power_thresh_bat_index();
    if (bi < 0) return 0;
    return (power_devs[bi].has_start_thresh ? 1 : 0) +
           (power_devs[bi].has_end_thresh ? 1 : 0);
}

static int power_can_manage_charge(void)
{
    const struct bat_device *pack = NULL;
    for (int i = 0; i < npower_devs; i++) {
        const struct bat_device *b = &power_devs[i];
        if (b->is_peripheral || b->is_offline_pack) continue;
        if (pack) return 0;
        pack = b;
    }
    return pack && pack->has_end_thresh && !strcmp(pack->end_attr, "charge_control_end_threshold") &&
           (!pack->has_start_thresh || !strcmp(pack->start_attr, "charge_control_start_threshold")) &&
           pack->serial[0] && strcmp(pack->serial, "0") && strcasecmp(pack->serial, "unknown");
}


static const char *power_full_unavailable(void)
{
    if (!power_can_manage_charge()) return _("Charging control unsupported");
    if (power_ac.system != BAT_AC_ONLINE) return _("Plug in AC power first");
    return NULL;
}

static int power_full_active(void)
{
    if (!power_charge_known || power_charge.full_until <= bat_now_ts()) return 0;
    for (int i = 0; i < npower_devs; i++) {
        const struct bat_device *b = &power_devs[i];
        if (b->is_peripheral || b->is_offline_pack) continue;
        int si = power_charge_slot_for(b);
        if (si >= 0 && !strcmp(power_charge.slots[si].state, "full")) return 1;
    }
    return 0;
}

static void power_charge_release_note(const struct bat_charge_slot_status *s)
{
    if (!rc_bat_adaptive || !power_charge.adaptive || power_full_pending ||
        power_ac.system != BAT_AC_ONLINE || strcmp(s->state, "hold") ||
        !power_can_manage_charge()) return;

    int64_t now = bat_now_ts();
    /* The policy projects at most one day ahead. A past release is not a forecast. */
    if (now <= 0 || s->release_ts <= now || s->release_ts - now > 24 * 60 * 60) return;
    time_t ts = (time_t)s->release_ts;
    struct tm tmv;
    char when[24], line[128];
    if ((int64_t)ts != s->release_ts || !localtime_r(&ts, &tmv) ||
        !strftime(when, sizeof when, "%m-%d %H:%M", &tmv)) return;
    snprintf(line, sizeof line, _("Expected to resume charging to full at %s"), when);
    add_entry(E_NOTE, line);
}

static void power_charge_notes(void)
{
    if (!power_charge_known) {
        if (power_can_manage_charge()) add_entry(E_NOTE, _("Charging status unavailable"));
        return;
    }
    int nslots = 0;
    for (int i = 0; i < npower_devs; i++) {
        const struct bat_device *b = &power_devs[i];
        if (b->is_peripheral || b->is_offline_pack) continue;
        int si = power_charge_slot_for(b);
        if (si < 0) continue;
        nslots++;
        const struct bat_charge_slot_status *s = &power_charge.slots[si];
        const char *state = power_charge_state_label(s->state);
        char line[128];
        if (s->actual_end > 0 && s->requested_end > 0 && s->actual_end != s->requested_end) {
            snprintf(line, sizeof line, _("%s: %s (limit %d%%, requested %d%%)"), b->name, state,
                     s->actual_end, s->requested_end);
        } else if (s->actual_end > 0) {
            snprintf(line, sizeof line, _("%s: %s (limit %d%%)"), b->name, state, s->actual_end);
        } else {
            snprintf(line, sizeof line, "%s: %s", b->name, state);
        }
        add_entry(E_NOTE, line);
        if (!strcmp(s->state, "model-missing") || !strcmp(s->state, "model-invalid")) {
            /* TRANSLATORS: r is the r key; keep it. */
            add_entry(E_NOTE, _("Press r on Adaptive Charging to download the model."));
        }
        power_charge_release_note(s);
    }
    if (!nslots && power_can_manage_charge()) add_entry(E_NOTE, _("Charging status unavailable"));
}

static int power_charge_managed(void)
{
    int64_t now = bat_now_ts();
    return rc_bat_adaptive || (power_charge_known && (power_charge.adaptive || power_charge.full_until > 0)) ||
           (rc_bat_full_until > now && rc_bat_full_until - now <= BAT_CHARGE_FULL_SECONDS &&
            (!power_charge_known || power_full_pending));
}

static const char *power_state_label(const char *status)
{
    if (!status || !status[0]) return _("Unknown");
    if (strcmp(status, "Charging") == 0) return _("Charging");
    if (strcmp(status, "Discharging") == 0) return _("Discharging");
    if (strcmp(status, "Full") == 0) return _("Full");
    if (strcmp(status, "Not charging") == 0) return _("Not charging");
    if (strcmp(status, "Idle") == 0) return _("Idle");
    if (strcmp(status, "Removed") == 0) return _("Removed (spare)");
    return status;
}

static const char *power_state_code_label(char code)
{
    switch (code) {
    case 'C': return _("Charging");
    case 'D': return _("Discharging");
    case 'F': return _("Full");
    case 'N': return _("Not charging");
    case 'R': return _("Removed from slot");
    default:  return _("Unknown");
    }
}

static void power_format_handoff(const struct bat_handoff_event *ev, char *out, size_t size)
{
    time_t tt = (time_t)ev->ts;
    struct tm tmv;
    localtime_r(&tt, &tmv);
    char tstr[24];
    strftime(tstr, sizeof tstr, "%m-%d %H:%M", &tmv);
    if (strcmp(ev->kind, "SWAP") == 0) {
        snprintf(out, size, _("%s %s swapped: %.24s (%d%%) -> %.24s (%d%%)"),
                 tstr, ev->slot, ev->pack_a, (ev->dpct_a + 5) / 10,
                 ev->pack_b, (ev->dpct_b + 5) / 10);
    } else if (strcmp(ev->kind, "DISCHARGE") == 0) {
        snprintf(out, size, _("%s discharge handoff %s (%d%% -> %d%%)"),
                 tstr, ev->slot, (ev->dpct_a + 5) / 10, (ev->dpct_b + 5) / 10);
    } else if (strcmp(ev->kind, "REMOVE") == 0) {
        snprintf(out, size, _("%s %s removed: %.28s (%d%%)"),
                 tstr, ev->slot, ev->pack_a, (ev->dpct_a + 5) / 10);
    } else if (strcmp(ev->kind, "INSERT") == 0) {
        snprintf(out, size, _("%s %s inserted: %.28s (%d%%)"),
                 tstr, ev->slot, ev->pack_b, (ev->dpct_b + 5) / 10);
    } else {
        snprintf(out, size, "%s %s %s", tstr, ev->kind, ev->slot);
    }
}

static void power_summary_text(char *out, size_t size)
{
    if (!power_loaded) power_refresh_devices();
    int live_cnt = 0, spare_cnt = 0;
    const struct bat_device *first = NULL;
    for (int i = 0; i < npower_devs; i++) {
        if (power_devs[i].is_peripheral) continue;
        if (power_devs[i].is_offline_pack) {
            spare_cnt++;
        } else {
            if (!first) first = &power_devs[i];
            live_cnt++;
        }
    }
    if (!first) {
        if (spare_cnt > 0) {
            snprintf(out, size, _("AC power (%d removed pack on record)   (Enter: curves & health)"),
                     spare_cnt);
        } else {
            snprintf(out, size, _("%s   (Enter: power & battery details)"),
                     power_ac.online ? _("AC power, no battery detected") : _("No battery detected"));
        }
        return;
    }
    char hbuf[40] = "", pbuf[32] = "", sbuf[32] = "";
    if (first->health_pct > 0) snprintf(hbuf, sizeof hbuf, _(", health %.1f%%"), first->health_pct);
    if (first->power_w >= 0.05) snprintf(pbuf, sizeof pbuf, " %.1f W", first->power_w);
    if (spare_cnt > 0) snprintf(sbuf, sizeof sbuf, _(" +%d spare"), spare_cnt);
    if (live_cnt > 1) {
        snprintf(out, size, _("%d batteries%s (%s %d%%, %s%s%s)   (Enter: curves & health)"),
                 live_cnt, sbuf, first->name, first->capacity_pct,
                 power_state_label(first->status), pbuf, hbuf);
    } else {
        snprintf(out, size, _("%s%s %d%%, %s%s%s   (Enter: curves & health)"),
                 first->name, sbuf, first->capacity_pct,
                 power_state_label(first->status), pbuf, hbuf);
    }
}

static void add_power_category_notes(void)
{
    if (!power_loaded) power_refresh_devices();
    char line[128];
    int any = 0;
    for (int i = 0; i < npower_devs; i++) {
        const struct bat_device *b = &power_devs[i];
        if (b->is_peripheral) continue;
        any = 1;
        char model[64] = "";
        if (b->vendor[0] && b->model[0]) snprintf(model, sizeof model, "%.20s %.24s", b->vendor, b->model);
        else if (b->model[0]) snprintf(model, sizeof model, "%.40s", b->model);
        else if (b->vendor[0]) snprintf(model, sizeof model, "%.40s", b->vendor);
        else snprintf(model, sizeof model, "%s", b->technology[0] ? b->technology : _("Battery"));

        const char *where = b->is_offline_pack ? _("Spare (removed)") : b->name;
        if (b->health_pct > 0 && b->energy_full_wh > 0 && b->energy_design_wh > 0) {
            if (b->cycles > 0) {
                snprintf(line, sizeof line, _("%s [%s]: health %.1f%% (%.1f / %.1f Wh), %d cycles"),
                         where, model, b->health_pct, b->energy_full_wh, b->energy_design_wh, b->cycles);
            } else {
                snprintf(line, sizeof line, _("%s [%s]: health %.1f%% (%.1f / %.1f Wh)"),
                         where, model, b->health_pct, b->energy_full_wh, b->energy_design_wh);
            }
        } else {
            snprintf(line, sizeof line, _("%s [%s]: %d%%, %s"),
                     where, model, b->capacity_pct, power_state_label(b->status));
        }
        add_entry(E_NOTE, line);
    }
    power_ensure_last_handoff();
    if (power_last_handoff[0]) {
        add_entry(E_NOTE, power_last_handoff);
    }
    if (!any) {
        add_entry(E_NOTE, power_ac.online ?
                  _("Running on AC adapter; no system battery is reported in /sys/class/power_supply.") :
                  _("No system battery is reported in /sys/class/power_supply."));
    }
}

static int power_charge_reload(void)
{
    if (kill(1, SIGHUP) == 0) return 0;
    snprintf(notice, sizeof notice, "%s", _("Saved, but could not notify the battery service."));
    return -1;
}

static int power_model_dir(char *out, size_t size)
{
    int n = snprintf(out, size, "%s/model", bat_history_dir());
    return n > 0 && (size_t)n < size ? 0 : -1;
}

static int power_model_file_ok(const char *dir, const char *name, size_t expected_size)
{
    char path[640];
    int n = snprintf(path, sizeof path, "%s/%s", dir, name);
    if (n <= 0 || (size_t)n >= sizeof path) return 0;
    struct stat st;
    if (lstat(path, &st) != 0) return 0;
    return S_ISREG(st.st_mode) && st.st_size >= 0 && (size_t)st.st_size == expected_size;
}

/* Fast local presence/size check (daemon performs full SHA-512 verification). */
static int power_model_files_present(void)
{
    char dir[560];
    if (power_model_dir(dir, sizeof dir) != 0) return 0;
    struct stat dst;
    if (lstat(dir, &dst) != 0 || !S_ISDIR(dst.st_mode)) return 0;
    return power_model_file_ok(dir, BAT_MODEL_TFLITE_NAME, BAT_MODEL_TFLITE_SIZE) &&
           power_model_file_ok(dir, BAT_MODEL_PREPROC_NAME, BAT_MODEL_PREPROC_SIZE);
}

static int power_model_any_present(void)
{
    char dir[560], path[640];
    if (power_model_dir(dir, sizeof dir) != 0) return 0;
    struct stat st;
    if (lstat(dir, &st) != 0 || !S_ISDIR(st.st_mode)) return 0;
    static const char *const names[] = {
        BAT_MODEL_TFLITE_NAME,
        BAT_MODEL_PREPROC_NAME,
        "manifest.installed.json",
    };
    for (size_t i = 0; i < sizeof names / sizeof names[0]; i++) {
        int n = snprintf(path, sizeof path, "%s/%s", dir, names[i]);
        if (n > 0 && (size_t)n < sizeof path && lstat(path, &st) == 0) return 1;
    }
    return 0;
}

static int power_charge_needs_model(void)
{
    if (!power_charge_known) return 0;
    for (int i = 0; i < npower_devs; i++) {
        const struct bat_device *b = &power_devs[i];
        if (b->is_peripheral || b->is_offline_pack) continue;
        int si = power_charge_slot_for(b);
        if (si >= 0 && (!strcmp(power_charge.slots[si].state, "model-missing") ||
                        !strcmp(power_charge.slots[si].state, "model-invalid"))) {
            return 1;
        }
    }
    return 0;
}

static void bat_model_check(void)
{
    struct jobs_row r;
    char log[96];
    if (!bat_model_job || !job_ended(bat_model_job, &r, log, sizeof log)) return;
    int removing = strcmp(bat_model_what, "remove") == 0;
    bat_model_job = 0;
    bat_model_what[0] = '\0';
    bat_model_remove_asked = 0;
    power_refresh_devices();
    entries_dirty = 1;
    full_redraw = 1;
    if (strcmp(r.state, "done") == 0) {
        snprintf(notice, sizeof notice, "%s", removing ?
                 _("Adaptive charging model removed.") :
                 _("Adaptive charging model downloaded and verified."));
    } else if (removing) {
        snprintf(notice, sizeof notice, _("Could not remove adaptive charging model: %s"),
                 r.last[0] ? r.last : log);
    } else {
        snprintf(notice, sizeof notice, _("Could not download adaptive charging model: %s"),
                 r.last[0] ? r.last : log);
    }
}

static void power_model_fetch_row(void)
{
    power_refresh_devices();
    if (!power_can_manage_charge()) {
        snprintf(notice, sizeof notice, "%s", _("Charging control unsupported"));
        return;
    }
    if (!rc_bat_adaptive) {
        snprintf(notice, sizeof notice, "%s", _("Turn Adaptive Charging on first to download the model."));
        return;
    }
    if (bat_model_job) {
        snprintf(notice, sizeof notice, "%s", _("Wait: an adaptive charging model job is still running."));
        return;
    }
    if (power_model_files_present() && !power_charge_needs_model()) {
        snprintf(notice, sizeof notice, "%s", _("Adaptive charging model is already installed."));
        return;
    }
    char why[160] = "";
    int id = 0, pos = 0;
    if (jobs_submit("battery-model-fetch", NULL, &id, &pos, why, sizeof why) < 0) {
        snprintf(notice, sizeof notice, _("Could not start model download: %s"), why);
        return;
    }
    bat_model_job = id;
    snprintf(bat_model_what, sizeof bat_model_what, "fetch");
    snprintf(notice, sizeof notice, "%s", _("Downloading adaptive charging model in background."));
}

static void power_model_remove_row(void)
{
    if (bat_model_job) {
        snprintf(notice, sizeof notice, "%s", _("Wait: an adaptive charging model job is still running."));
        return;
    }
    if (!power_model_any_present()) {
        bat_model_remove_asked = 0;
        snprintf(notice, sizeof notice, "%s", _("Adaptive charging model is not installed."));
        return;
    }
    if (!bat_model_remove_asked) {
        bat_model_remove_asked = 1;
        /* TRANSLATORS: Delete is the Delete key; keep it. */
        snprintf(notice, sizeof notice, "%s", _("Delete again: remove the downloaded adaptive charging model."));
        return;
    }
    bat_model_remove_asked = 0;
    char why[160] = "";
    int id = 0, pos = 0;
    if (jobs_submit("battery-model-remove", NULL, &id, &pos, why, sizeof why) < 0) {
        snprintf(notice, sizeof notice, _("Could not remove adaptive charging model: %s"), why);
        return;
    }
    bat_model_job = id;
    snprintf(bat_model_what, sizeof bat_model_what, "remove");
    snprintf(notice, sizeof notice, "%s", _("Removing adaptive charging model in background."));
}

static void adjust_adaptive_charging(void)
{
    power_refresh_devices();
    if (!rc_bat_adaptive && !power_can_manage_charge()) {
        snprintf(notice, sizeof notice, "%s", _("Charging control unsupported"));
        return;
    }
    int next = !rc_bat_adaptive;
    if (rc_set("battery_charge_adaptive", next ? "1" : "0") < 0) {
        snprintf(notice, sizeof notice, _("Cannot write %s."), RC_CONF);
        return;
    }
    rc_bat_adaptive = next;
    int reloaded = power_charge_reload() == 0;
    if (next == 1 && !power_model_files_present()) {
        char why[160] = "";
        int id = 0, pos = 0;
        if (bat_model_job && strcmp(bat_model_what, "fetch") == 0) {
            snprintf(notice, sizeof notice, "%s", _("Adaptive charging enabled; downloading model in background."));
        } else if (jobs_submit("battery-model-fetch", NULL, &id, &pos, why, sizeof why) == 0) {
            bat_model_job = id;
            snprintf(bat_model_what, sizeof bat_model_what, "fetch");
            snprintf(notice, sizeof notice, "%s", _("Adaptive charging enabled; downloading model in background."));
        } else {
            snprintf(notice, sizeof notice, _("Adaptive charging enabled; model download unavailable: %s"), why);
        }
        return;
    }
    if (reloaded) {
        snprintf(notice, sizeof notice, "%s", _("Adaptive charging setting saved."));
    }
}

static void charge_to_full_now(void)
{
    power_refresh_devices();
    const char *why = power_full_unavailable();
    if (why) {
        snprintf(notice, sizeof notice, "%s", why);
        return;
    }
    if (power_full_active()) {
        snprintf(notice, sizeof notice, "%s", _("Full charge override active"));
        return;
    }
    int64_t until = bat_now_ts() + BAT_CHARGE_FULL_SECONDS;
    char val[32];
    snprintf(val, sizeof val, "%lld", (long long)until);
    if (rc_set("battery_charge_full_until", val) < 0) {
        snprintf(notice, sizeof notice, _("Cannot write %s."), RC_CONF);
        return;
    }
    rc_bat_full_until = until;
    power_full_pending = 1;
    if (power_charge_reload() == 0) {
        snprintf(notice, sizeof notice, "%s", _("Full charge requested until unplugged or for up to 24 hours."));
    }
}

static void adjust_power_threshold(int which, int dir)
{
    if (!power_loaded) power_refresh_devices();
    power_refresh_charge_status();
    if (power_charge_managed()) {
        snprintf(notice, sizeof notice, "%s", _("Manual thresholds are saved but not applied while charging is managed."));
        return;
    }
    int bi = power_thresh_bat_index();
    if (bi < 0) return;
    const struct bat_device *b = &power_devs[bi];
    int is_end = which == 1 || !b->has_start_thresh;
    int start = rc_bat_start >= 0 ? rc_bat_start : b->start_thresh;
    int end = rc_bat_end >= 0 ? rc_bat_end : b->end_thresh;
    int current = is_end ? end : start;
    int lo = is_end ? (b->has_start_thresh && start + 5 > 20 ? start + 5 : 20) : 0;
    int hi = is_end ? 100 : (b->has_end_thresh ? end - 5 : 95);
    if (lo > hi) return;
    int next = current + dir * 5;
    if (next < lo) next = lo;
    if (next > hi) next = hi;
    if (next == current) return;
    const char *key = is_end ? "battery_charge_end" : "battery_charge_start";
    char val[16];
    snprintf(val, sizeof val, "%d", next);
    if (rc_set(key, val) != 0) {
        snprintf(notice, sizeof notice, _("Cannot write %s."), RC_CONF);
        return;
    }
    if (is_end) rc_bat_end = next;
    else rc_bat_start = next;
    if (power_charge_reload() == 0)
        snprintf(notice, sizeof notice, "%s", _("Waiting for battery service"));
}

/* --- Interactive Energy & Battery Curves Subpage --- */

enum { PSPAN_1H, PSPAN_2H, PSPAN_12H, PSPAN_24H, PSPAN_48H, PSPAN_7D, NPSPANS };
static const int power_span_secs[NPSPANS] = {
    3600, 2 * 3600, 12 * 3600, 24 * 3600, 48 * 3600, 7 * 86400
};
static const char *const power_span_labels[NPSPANS] = {
    N_("1h"), N_("2h"), N_("12h"), N_("24h"), N_("48h"), N_("7d")
};
static const char *const power_span_titles[NPSPANS] = {
    N_("Last hour"), N_("Last 2 hours"), N_("Last 12 hours"),
    N_("Last 24 hours"), N_("Last 48 hours"), N_("Last 7 days")
};

struct power_hit {
    int x, y, w, h;
    int kind; /* 1: dev, 2: mode, 3: span, 4: refresh, 5: copy, 6: graph */
    int val;
};

static struct bat_sample power_samples[BAT_MAX_SAMPLES];
static int npower_samples;
static struct bat_handoff_event power_handoffs[BAT_MAX_EVENTS];
static int npower_handoffs;
static int power_sel_dev;
static int power_mode;      /* 0: Charge %, 1: Power (W) */
static int power_span = PSPAN_12H;
static int power_cursor = -1;
static int power_detail_scroll, power_detail_max;
static struct power_hit power_hits[32];
static int npower_hits;
static int64_t power_now_override;

static int64_t power_now_ts(void)
{
    return power_now_override > 0 ? power_now_override : (int64_t)time(NULL);
}

static void power_add_hit(int x, int y, int w, int h, int kind, int val)
{
    if (npower_hits < (int)(sizeof power_hits / sizeof power_hits[0]) && w > 0 && h > 0) {
        power_hits[npower_hits++] = (struct power_hit){ x, y, w, h, kind, val };
    }
}

static void power_load_selected_history(void)
{
    npower_samples = 0;
    int64_t now_ts = power_now_ts();
    int64_t since = now_ts - power_span_secs[power_span];
    npower_handoffs = bat_load_handoffs(since, power_handoffs, BAT_MAX_EVENTS);

    if (npower_devs <= 0) return;
    if (power_sel_dev < 0) power_sel_dev = 0;
    if (power_sel_dev >= npower_devs) power_sel_dev = npower_devs - 1;
    const struct bat_device *b = &power_devs[power_sel_dev];
    if (b->is_peripheral) return;

    npower_samples = bat_load_history(b->pack_id, b->is_offline_pack ? NULL : b->name,
                                      since, power_samples, BAT_MAX_SAMPLES - 1);

    /* Append live instant point only when the pack is currently inserted! */
    if (!b->is_offline_pack && now_ts >= 946684800LL && npower_samples < BAT_MAX_SAMPLES) {
        struct bat_sample live;
        bat_sample_from_device(b, power_ac.online, now_ts, &live);
        if (npower_samples == 0 || now_ts > power_samples[npower_samples - 1].ts) {
            power_samples[npower_samples++] = live;
        } else if (now_ts == power_samples[npower_samples - 1].ts) {
            power_samples[npower_samples - 1] = live;
        }
    }
}

static void px_vline_clamped(int x, int y0, int y1, int clip_y0, int clip_y1, uint32_t rgb)
{
    if (y0 > y1) { int t = y0; y0 = y1; y1 = t; }
    if (y1 < clip_y0 || y0 > clip_y1) return;
    if (y0 < clip_y0) y0 = clip_y0;
    if (y1 > clip_y1) y1 = clip_y1;
    screen_fill_px(x, y0, 1, y1 - y0 + 1, rgb);
}

static void power_format_eta(const struct bat_device *b, char *out, size_t size)
{
    out[0] = '\0';
    if (b->is_offline_pack || b->power_w < 0.2 || b->energy_now_wh < 0) return;
    double hours = -1.0;
    if (strcmp(b->status, "Discharging") == 0) {
        hours = b->energy_now_wh / b->power_w;
    } else if (strcmp(b->status, "Charging") == 0 && b->energy_full_wh > b->energy_now_wh) {
        double target_wh = b->energy_full_wh;
        if (b->has_end_thresh && b->end_thresh > 0 && b->end_thresh < 100) {
            double capped = b->energy_full_wh * (b->end_thresh / 100.0);
            if (capped > b->energy_now_wh) target_wh = capped;
        }
        hours = (target_wh - b->energy_now_wh) / b->power_w;
    }
    if (hours > 0.01 && hours < 99.0) {
        int mins = (int)(hours * 60.0 + 0.5);
        snprintf(out, size, _(" (%dh %02dm remaining)"), mins / 60, mins % 60);
    }
}

static void power_build_clipboard_report(char *buf, size_t size)
{
    size_t pos = 0;
    pos += (size_t)snprintf(buf + pos, size - pos, "=== Power & Battery ===\nAC adapter: %s",
                            power_ac.online ? "online" : "offline");
    if (power_ac.voltage_v > 0 && pos < size)
        pos += (size_t)snprintf(buf + pos, size - pos, " (%.2f V)", power_ac.voltage_v);
    if (pos < size) pos += (size_t)snprintf(buf + pos, size - pos, "\n");
    for (int i = 0; i < npower_devs && pos + 160 < size; i++) {
        const struct bat_device *b = &power_devs[i];
        pos += (size_t)snprintf(buf + pos, size - pos,
                                "\n[%s — %s]%s\n  Model: %s %s (S/N %s)\n  State: %s (%d.%d%%)\n",
                                b->is_offline_pack ? "Spare" : b->name, b->pack_id,
                                b->is_peripheral ? " (peripheral)" : (b->is_offline_pack ? " (removed)" : ""),
                                b->vendor, b->model, b->serial[0] ? b->serial : "-",
                                b->status, b->capacity_decipct / 10, b->capacity_decipct % 10);
        if (b->health_pct > 0 && pos < size)
            pos += (size_t)snprintf(buf + pos, size - pos, "  Health: %.1f%%\n", b->health_pct);
        if (b->cycles > 0 && pos < size)
            pos += (size_t)snprintf(buf + pos, size - pos, "  Cycles: %d\n", b->cycles);
        if (b->energy_now_wh >= 0 && b->energy_full_wh > 0 && pos < size)
            pos += (size_t)snprintf(buf + pos, size - pos,
                                    "  Energy: %.2f / %.2f Wh (design %.2f Wh)\n",
                                    b->energy_now_wh, b->energy_full_wh,
                                    b->energy_design_wh > 0 ? b->energy_design_wh : 0.0);
        if (b->power_w >= 0 && pos < size)
            pos += (size_t)snprintf(buf + pos, size - pos, "  Power: %.2f W\n", b->power_w);
    }
    if (npower_handoffs > 0 && pos + 128 < size) {
        pos += (size_t)snprintf(buf + pos, size - pos, "\nRecent handoffs:\n");
        int start = npower_handoffs > 5 ? npower_handoffs - 5 : 0;
        for (int i = start; i < npower_handoffs && pos + 160 < size; i++) {
            char h[256];
            power_format_handoff(&power_handoffs[i], h, sizeof h);
            pos += (size_t)snprintf(buf + pos, size - pos, "  %.140s\n", h);
        }
    }
}

/* Query sample at timestamp q_ts:
 *   *gap_kind = 0: active inserted interval
 *   *gap_kind = 1: system sleep / off gap while pack stayed inserted
 *   *gap_kind = 2: pack was physically removed from slot ('R') */
static int power_query_at(int64_t q_ts, int64_t gap_sec, struct bat_sample *out, int *gap_kind)
{
    *gap_kind = 0;
    if (npower_samples <= 0) return 0;
    if (q_ts < power_samples[0].ts - 90) return 0;
    if (q_ts <= power_samples[0].ts) {
        if (power_samples[0].state == 'R') { *gap_kind = 2; *out = power_samples[0]; return 1; }
        *out = power_samples[0];
        return 1;
    }
    int last = npower_samples - 1;
    if (q_ts >= power_samples[last].ts) {
        if (power_samples[last].state == 'R') {
            *out = power_samples[last];
            *gap_kind = 2;
            return 1;
        }
        if (q_ts - power_samples[last].ts > 120) return 0;
        *out = power_samples[last];
        return 1;
    }
    int lo = 0, hi = last;
    while (lo + 1 < hi) {
        int mid = (lo + hi) / 2;
        if (power_samples[mid].ts <= q_ts) lo = mid;
        else hi = mid;
    }
    const struct bat_sample *a = &power_samples[lo], *b = &power_samples[hi];
    if (a->state == 'R') {
        *out = *a;
        out->ts = q_ts;
        *gap_kind = 2;
        return 1;
    }
    int64_t dt = b->ts - a->ts;
    double t = dt > 0 ? (double)(q_ts - a->ts) / (double)dt : 0.0;
    *out = t < 0.5 ? *a : *b;
    if (out->state == 'R') out->state = a->state;
    out->ts = q_ts;
    out->decipct = (int)(a->decipct + (b->decipct - a->decipct) * t + 0.5);
    out->power_mw = (int)(a->power_mw + (b->power_mw - a->power_mw) * t + 0.5);
    out->voltage_mv = (int)(a->voltage_mv + (b->voltage_mv - a->voltage_mv) * t + 0.5);
    if (dt > gap_sec) *gap_kind = 1;
    return 1;
}

static int power_handoff_relates_to(const struct bat_handoff_event *ev, const struct bat_device *b)
{
    if (!b) return 0;
    return strcmp(ev->pack_a, b->pack_id) == 0 ||
           strcmp(ev->pack_b, b->pack_id) == 0 ||
           (!b->is_offline_pack && strncmp(ev->slot, b->name, strlen(b->name)) == 0);
}

static void power_curves_draw(void)
{
    int cols = screen_cols(), rows = screen_rows();
    npower_hits = 0;
    power_detail_max = 0;
    ui_hits_clear();
    screen_fill(0, 0, cols, rows, BG);
    screen_bar(_("settings — Power & Battery"), NULL);
    settings_back_button(rows - 1);

    if (cols < 36 || rows < 14) {
        power_detail_scroll = 0;
        screen_text(1, 2, _("Window too small for battery curves."), FG, BG);
        screen_flush();
        return;
    }

    /* Row 2: Battery pack selector tabs (inserted + offline spares + peripherals) + AC */
    int x = 2;
    if (npower_devs == 0) {
        screen_text(x, 2, _("No battery detected"), DIM, BG);
    } else {
        for (int i = 0; i < npower_devs; i++) {
            const struct bat_device *b = &power_devs[i];
            char tab[96];
            if (b->is_peripheral) {
                snprintf(tab, sizeof tab, " %.18s (%s) %d%% ",
                         b->model[0] ? b->model : b->name, _("Device"), b->capacity_pct);
            } else if (b->is_offline_pack) {
                snprintf(tab, sizeof tab, " %s: %.16s %d%% ",
                         _("Spare"), b->model[0] ? b->model : b->pack_id, b->capacity_pct);
            } else if (b->model[0]) {
                snprintf(tab, sizeof tab, " %s (%.14s) %d%% ", b->name, b->model, b->capacity_pct);
            } else {
                snprintf(tab, sizeof tab, " %s %d%% ", b->name, b->capacity_pct);
            }
            int w = ui_cells(tab);
            if (x + w >= cols - 20) break;
            int sel = (i == power_sel_dev);
            uint32_t bg = sel ? ACCENT : SIDE_BG;
            uint32_t fg = sel ? BG : (b->is_offline_pack ? POWER_WARN_COL : FG);
            screen_fill(x, 2, w, 1, bg);
            screen_text(x, 2, tab, fg, bg);
            power_add_hit(x, 2, w, 1, 1, i);
            x += w + 1;
        }
    }
    char ac_tag[64];
    if (power_ac.online && power_ac.voltage_v > 0)
        snprintf(ac_tag, sizeof ac_tag, _("AC: On (%.1f V)"), power_ac.voltage_v);
    else
        snprintf(ac_tag, sizeof ac_tag, "%s", power_ac.online ? _("AC: Online") : _("AC: Offline"));
    int acw = ui_cells(ac_tag);
    if (cols - 2 - acw > x) {
        screen_text(cols - 2 - acw, 2, ac_tag, power_ac.online ? POWER_GOOD_COL : DIM, BG);
    }

    /* Row 3: Graph mode buttons, 6 KDE timespans, Refresh, Copy */
    x = 2;
    const char *modes[2] = { _(" Charge (%) "), _(" Power (W) ") };
    for (int m = 0; m < 2; m++) {
        int w = ui_cells(modes[m]);
        uint32_t bg = (power_mode == m) ? ACCENT : SIDE_BG;
        screen_fill(x, 3, w, 1, bg);
        screen_text(x, 3, modes[m], power_mode == m ? BG : FG, bg);
        power_add_hit(x, 3, w, 1, 2, m);
        x += w + 1;
    }
    x += 2;
    screen_text(x, 3, _("Span:"), DIM, BG);
    x += ui_cells(_("Span:")) + 1;
    for (int s = 0; s < NPSPANS; s++) {
        char btn[24];
        snprintf(btn, sizeof btn, " %s ", _(power_span_labels[s]));
        int w = ui_cells(btn);
        if (x + w >= cols - 2) break;
        uint32_t bg = (power_span == s) ? ACCENT : SIDE_BG;
        screen_fill(x, 3, w, 1, bg);
        screen_text(x, 3, btn, power_span == s ? BG : FG, bg);
        power_add_hit(x, 3, w, 1, 3, s);
        x += w + 1;
    }
    const char *rbtn = _(" Refresh ");
    int rw = ui_cells(rbtn);
    if (x + rw + 2 < cols - 2) {
        x += 1;
        screen_fill(x, 3, rw, 1, SIDE_BG);
        screen_text(x, 3, rbtn, FG, SIDE_BG);
        power_add_hit(x, 3, rw, 1, 4, 0);
        x += rw + 1;
    }
    const char *cbtn = _(" Copy ");
    int cpw = ui_cells(cbtn);
    if (x + cpw < cols - 2) {
        screen_fill(x, 3, cpw, 1, SIDE_BG);
        screen_text(x, 3, cbtn, FG, SIDE_BG);
        power_add_hit(x, 3, cpw, 1, 5, 0);
    }

    int graph_top = 5;
    int graph_rows = (rows - 8) / 2;
    if (graph_rows < 7) graph_rows = 7;
    if (graph_rows > 16) graph_rows = 16;
    if (graph_top + graph_rows > rows - 7) graph_rows = rows - 7 - graph_top;
    int card_x = 2, card_w = cols - 4;
    int plot_x = card_x + 8, plot_w = card_w - 10;
    int plot_y = graph_top + 1, plot_h = graph_rows - 2;
    if (plot_w < 10) plot_w = 10;
    if (plot_h < 3) plot_h = 3;

    screen_fill(card_x, graph_top, card_w, graph_rows, POWER_CARD_BG);
    power_add_hit(plot_x, plot_y, plot_w, plot_h, 6, 0);

    const struct bat_device *sel_bat = (npower_devs > 0 && power_sel_dev >= 0 && power_sel_dev < npower_devs)
                                       ? &power_devs[power_sel_dev] : NULL;

    double max_y = (power_mode == 0) ? 100.0 : 15.0;
    double sum_w = 0.0, peak_w = 0.0;
    int cnt_w = 0;
    for (int i = 0; i < npower_samples; i++) {
        if (power_samples[i].state == 'R') continue;
        double w = (double)power_samples[i].power_mw / 1000.0;
        if (w > peak_w) peak_w = w;
        sum_w += w;
        cnt_w++;
    }
    if (power_mode == 1) {
        if (peak_w > 12.0) max_y = ((int)(peak_w / 10.0) + 1) * 10.0;
        if (max_y < 10.0) max_y = 10.0;
    }

    int64_t end_ts = power_now_ts();
    int64_t span_sec = power_span_secs[power_span];
    int64_t start_ts = end_ts - span_sec;
    int64_t gap_sec = span_sec / 24;
    if (gap_sec < 300) gap_sec = 300;
    if (power_cursor >= plot_w) power_cursor = plot_w - 1;

    /* Graph header */
    char hdr[200];
    if (!sel_bat) {
        snprintf(hdr, sizeof hdr, "%s", _("No battery available"));
    } else if (sel_bat->is_peripheral) {
        snprintf(hdr, sizeof hdr, _("%s — Peripheral device (live battery level: %d%%, history not recorded)"),
                 sel_bat->model[0] ? sel_bat->model : sel_bat->name, sel_bat->capacity_pct);
    } else if (power_cursor >= 0 && npower_samples > 0) {
        int64_t q_ts = start_ts + (int64_t)((double)power_cursor / (double)(plot_w > 1 ? plot_w - 1 : 1) * (double)span_sec);
        int64_t tol = span_sec / (plot_w > 1 ? plot_w : 1) + 60;
        const struct bat_handoff_event *near_ev = NULL;
        for (int i = 0; i < npower_handoffs; i++) {
            int64_t d = power_handoffs[i].ts - q_ts;
            if (d < 0) d = -d;
            if (d <= tol && power_handoff_relates_to(&power_handoffs[i], sel_bat))
                near_ev = &power_handoffs[i];
        }
        if (near_ev) {
            char htxt[256];
            power_format_handoff(near_ev, htxt, sizeof htxt);
            snprintf(hdr, sizeof hdr, _("Handoff %.160s"), htxt);
        } else {
            struct bat_sample cs;
            int gap_kind = 0;
            time_t tt = (time_t)q_ts;
            struct tm tmv;
            localtime_r(&tt, &tmv);
            char tstr[32];
            strftime(tstr, sizeof tstr, span_sec > 86400 ? "%m-%d %H:%M" : "%H:%M", &tmv);
            if (power_query_at(q_ts, gap_sec, &cs, &gap_kind)) {
                if (gap_kind == 2) {
                    snprintf(hdr, sizeof hdr, _("Cursor %s: [%s — last %d.%d%%]"),
                             tstr, _("Removed from slot"), cs.decipct / 10, cs.decipct % 10);
                } else {
                    snprintf(hdr, sizeof hdr, _("Cursor %s: %d.%d%%  %.2f W  %.2f V  [%s%s]"),
                             tstr, cs.decipct / 10, cs.decipct % 10,
                             (double)cs.power_mw / 1000.0, (double)cs.voltage_mv / 1000.0,
                             power_state_code_label(cs.state),
                             gap_kind == 1 ? _(" — sleep/off gap") : "");
                }
            } else {
                snprintf(hdr, sizeof hdr, _("Cursor %s: no data recorded at this time"), tstr);
            }
        }
    } else if (cnt_w > 0) {
        snprintf(hdr, sizeof hdr, _("%s — %s [%s] (%d pts, avg %.1f W, peak %.1f W)"),
                 power_mode == 0 ? _("Charge Percentage") : _("Energy Consumption"),
                 _(power_span_titles[power_span]), sel_bat->pack_id,
                 cnt_w, sum_w / cnt_w, peak_w);
    } else {
        snprintf(hdr, sizeof hdr, _("%s — %s [%s]"),
                 power_mode == 0 ? _("Charge Percentage") : _("Energy Consumption"),
                 _(power_span_titles[power_span]), sel_bat->pack_id);
    }
    screen_text(card_x + 2, graph_top, hdr, ACCENT, POWER_CARD_BG);

    for (int tick = 0; tick <= 2; tick++) {
        int r = plot_y + (tick * (plot_h - 1)) / 2;
        double val = max_y * (1.0 - tick * 0.5);
        char ylab[16];
        if (power_mode == 0) snprintf(ylab, sizeof ylab, "%3.0f%%", val);
        else snprintf(ylab, sizeof ylab, "%4.0fW", val);
        screen_text(card_x + 1, r, ylab, DIM, POWER_CARD_BG);
    }

    int cw = 0, ch = 0, top_px = 0;
    if (screen_cells && screen_fill_px) screen_cells(&cw, &ch, &top_px);

    if (cw > 0 && ch > 0 && screen_fill_px && sel_bat && !sel_bat->is_peripheral && npower_samples > 0) {
        int px0 = plot_x * cw;
        int py0 = top_px + plot_y * ch + 2;
        int pw = plot_w * cw;
        int ph = plot_h * ch - 4;
        if (ph < 8) ph = 8;
        int py1 = py0 + ph - 1;

        for (int g = 0; g <= 4; g++) {
            int gy = py0 + (g * (ph - 1)) / 4;
            screen_fill_px(px0, gy, pw, 1, POWER_GRID_COL);
        }
        for (int g = 0; g <= 4; g++) {
            int gx = px0 + (g * (pw - 1)) / 4;
            screen_fill_px(gx, py0, 1, ph, POWER_GRID_COL);
        }

        int prev_valid = 0, prev_y = 0;
        for (int dx = 0; dx < pw; dx++) {
            int64_t q_ts = start_ts + (int64_t)((double)dx / (double)(pw > 1 ? pw - 1 : 1) * (double)span_sec);
            struct bat_sample cs;
            int gap_kind = 0;
            if (!power_query_at(q_ts, gap_sec, &cs, &gap_kind) || gap_kind == 2) {
                /* Uninserted ('R') or before first record: open break, no false line! */
                prev_valid = 0;
                continue;
            }
            double val = (power_mode == 0) ? ((double)cs.decipct / 10.0)
                                           : ((double)cs.power_mw / 1000.0);
            if (val < 0) val = 0;
            if (val > max_y) val = max_y;
            int y = py1 - (int)((val / max_y) * (ph - 1) + 0.5);

            if (gap_kind == 1) {
                if ((dx / 5) % 2 == 0)
                    px_vline_clamped(px0 + dx, prev_valid ? prev_y : y, y, py0, py1, POWER_GAP_COL);
            } else {
                uint32_t line_col = POWER_DIS_COL, fill_col = POWER_DIS_FILL;
                if (cs.state == 'C') {
                    line_col = POWER_CHG_COL;
                    fill_col = POWER_CHG_FILL;
                } else if (cs.state == 'F' || cs.state == 'N') {
                    line_col = POWER_IDLE_COL;
                    fill_col = POWER_DIS_FILL;
                } else if (cs.decipct <= 200) {
                    line_col = POWER_LOW_COL;
                    fill_col = POWER_LOW_FILL;
                }
                if (y < py1) screen_fill_px(px0 + dx, y + 1, 1, py1 - y, fill_col);
                px_vline_clamped(px0 + dx, prev_valid ? prev_y : y, y, py0, py1, line_col);
                if (y > py0) screen_fill_px(px0 + dx, y - 1, 1, 1, line_col);
            }
            prev_y = y;
            prev_valid = 1;
        }

        /* Draw vertical handoff ticks (swap / remove / insert / discharge handoff) */
        for (int i = 0; i < npower_handoffs; i++) {
            const struct bat_handoff_event *ev = &power_handoffs[i];
            if (ev->ts < start_ts || ev->ts > end_ts || !power_handoff_relates_to(ev, sel_bat))
                continue;
            int hx = px0 + (int)((double)(ev->ts - start_ts) / (double)span_sec * (pw - 1) + 0.5);
            for (int hy = py0; hy <= py1; hy += 4)
                screen_fill_px(hx, hy, 2, hy + 2 <= py1 ? 2 : 1, POWER_HOFF_COL);
        }

        if (power_cursor >= 0 && power_cursor < plot_w) {
            int cx_px = px0 + power_cursor * cw + cw / 2;
            screen_fill_px(cx_px, py0, 1, ph, POWER_CUR_COL);
        }
    } else if (sel_bat && !sel_bat->is_peripheral && npower_samples > 0) {
        /* Character-cell curve rendering (unit-test & non-pixel fallback) */
        for (int col = 0; col < plot_w; col++) {
            int64_t q_ts = start_ts + (int64_t)((double)col / (double)(plot_w > 1 ? plot_w - 1 : 1) * (double)span_sec);
            struct bat_sample cs;
            int gap_kind = 0;
            if (!power_query_at(q_ts, gap_sec, &cs, &gap_kind) || gap_kind == 2) {
                if (col == power_cursor) {
                    for (int rr = plot_y; rr < plot_y + plot_h; rr++)
                        screen_text(plot_x + col, rr, "|", POWER_CUR_COL, POWER_CARD_BG);
                }
                continue;
            }
            double val = (power_mode == 0) ? ((double)cs.decipct / 10.0)
                                           : ((double)cs.power_mw / 1000.0);
            if (val < 0) val = 0;
            if (val > max_y) val = max_y;
            int r = plot_y + (plot_h - 1) - (int)((val / max_y) * (plot_h - 1) + 0.5);
            uint32_t fg = gap_kind == 1 ? POWER_GAP_COL : (cs.state == 'C' ? POWER_CHG_COL : POWER_DIS_COL);
            screen_text(plot_x + col, r, gap_kind == 1 ? "." : "*", fg, POWER_CARD_BG);
            if (col == power_cursor) {
                for (int rr = plot_y; rr < plot_y + plot_h; rr++) {
                    if (rr != r) screen_text(plot_x + col, rr, "|", POWER_CUR_COL, POWER_CARD_BG);
                }
            }
        }
        for (int i = 0; i < npower_handoffs; i++) {
            const struct bat_handoff_event *ev = &power_handoffs[i];
            if (ev->ts < start_ts || ev->ts > end_ts || !power_handoff_relates_to(ev, sel_bat))
                continue;
            int hc = (int)((double)(ev->ts - start_ts) / (double)span_sec * (plot_w - 1) + 0.5);
            if (hc >= 0 && hc < plot_w)
                screen_text(plot_x + hc, plot_y, "H", POWER_HOFF_COL, POWER_CARD_BG);
        }
    }

    int axis_row = graph_top + graph_rows - 1;
    for (int t = 0; t <= 4; t++) {
        int64_t ts = start_ts + (span_sec * t) / 4;
        time_t tt = (time_t)ts;
        struct tm tmv;
        localtime_r(&tt, &tmv);
        char xlbl[24];
        if (t == 4) snprintf(xlbl, sizeof xlbl, "%s", _("Now"));
        else strftime(xlbl, sizeof xlbl, span_sec > 86400 ? "%m-%d %H:%M" : "%H:%M", &tmv);
        int lw = ui_cells(xlbl);
        int tx = plot_x + (t * (plot_w - 1)) / 4 - (t == 4 ? lw - 1 : (t == 0 ? 0 : lw / 2));
        if (tx < plot_x) tx = plot_x;
        if (tx + lw > card_x + card_w - 1) tx = card_x + card_w - 1 - lw;
        screen_text(tx, axis_row, xlbl, DIM, POWER_CARD_BG);
    }

    /* Detail sections below graph */
    int det_top = graph_top + graph_rows + 1;
    int det_rows = rows - 3 - det_top;
    if (det_rows >= 2 && sel_bat) {
        char eta[48];
        power_format_eta(sel_bat, eta, sizeof eta);

        struct { const char *k; char v[160]; uint32_t col; } g1[7], g2[6], g3[6];
        int n1 = 0, n2 = 0, n3 = 0;

        g1[n1].k = _("State");
        if (sel_bat->is_offline_pack && sel_bat->last_seen_ts > 0) {
            time_t tt = (time_t)sel_bat->last_seen_ts;
            struct tm tmv;
            localtime_r(&tt, &tmv);
            char ls[24];
            strftime(ls, sizeof ls, "%m-%d %H:%M", &tmv);
            snprintf(g1[n1].v, sizeof g1[0].v, _("Removed (last in %.24s at %.20s)"), sel_bat->name, ls);
            g1[n1].col = POWER_WARN_COL;
        } else {
            snprintf(g1[n1].v, sizeof g1[0].v, "%.40s%.40s", power_state_label(sel_bat->status), eta);
            g1[n1].col = FG;
        }
        n1++;

        g1[n1].k = sel_bat->is_offline_pack ? _("Last charge") : _("Current charge");
        snprintf(g1[n1].v, sizeof g1[0].v, "%d.%d%%", sel_bat->capacity_decipct / 10, sel_bat->capacity_decipct % 10);
        g1[n1++].col = FG;

        g1[n1].k = _("Health");
        if (sel_bat->health_pct > 0) {
            snprintf(g1[n1].v, sizeof g1[0].v, "%.1f%%", sel_bat->health_pct);
            g1[n1].col = sel_bat->health_pct >= 80.0 ? POWER_GOOD_COL :
                         (sel_bat->health_pct >= 60.0 ? POWER_WARN_COL : POWER_LOW_COL);
        } else {
            snprintf(g1[n1].v, sizeof g1[0].v, "%s", _("not reported"));
            g1[n1].col = DIM;
        }
        n1++;

        g1[n1].k = _("Charge cycles");
        if (sel_bat->cycles > 0) {
            snprintf(g1[n1].v, sizeof g1[0].v, "%d", sel_bat->cycles);
            g1[n1].col = FG;
        } else {
            snprintf(g1[n1].v, sizeof g1[0].v, "%s", _("not reported"));
            g1[n1].col = DIM;
        }
        n1++;

        g1[n1].k = _("Vendor / Model");
        snprintf(g1[n1].v, sizeof g1[0].v, "%.32s %.44s",
                 sel_bat->vendor[0] ? sel_bat->vendor : "—",
                 sel_bat->model[0] ? sel_bat->model : "");
        g1[n1++].col = FG;

        g1[n1].k = _("Serial / Type");
        snprintf(g1[n1].v, sizeof g1[0].v, "%.44s (%.24s)",
                 sel_bat->serial[0] ? sel_bat->serial : "—",
                 sel_bat->technology[0] ? sel_bat->technology : _("Li-ion"));
        g1[n1++].col = FG;

        /* Group 2: Energy */
        g2[n2].k = _("Power rate");
        if (!sel_bat->is_offline_pack && sel_bat->power_w >= 0) {
            snprintf(g2[n2].v, sizeof g2[0].v, "%.2f W", sel_bat->power_w);
            g2[n2].col = FG;
        } else {
            snprintf(g2[n2].v, sizeof g2[0].v, "%s", sel_bat->is_offline_pack ? _("offline") : _("not reported"));
            g2[n2].col = DIM;
        }
        n2++;

        g2[n2].k = _("Voltage");
        if (sel_bat->voltage_v > 0 && sel_bat->voltage_design_v > 0) {
            snprintf(g2[n2].v, sizeof g2[0].v, _("%.2f V (design %.2f V)"),
                     sel_bat->voltage_v, sel_bat->voltage_design_v);
            g2[n2].col = FG;
        } else if (sel_bat->voltage_v > 0) {
            snprintf(g2[n2].v, sizeof g2[0].v, "%.2f V", sel_bat->voltage_v);
            g2[n2].col = FG;
        } else if (sel_bat->voltage_design_v > 0) {
            snprintf(g2[n2].v, sizeof g2[0].v, _("design %.2f V"), sel_bat->voltage_design_v);
            g2[n2].col = DIM;
        } else {
            snprintf(g2[n2].v, sizeof g2[0].v, "%s", _("not reported"));
            g2[n2].col = DIM;
        }
        n2++;

        g2[n2].k = _("Remaining energy");
        if (sel_bat->energy_now_wh >= 0) {
            snprintf(g2[n2].v, sizeof g2[0].v, "%.2f Wh", sel_bat->energy_now_wh);
            g2[n2].col = FG;
        } else {
            snprintf(g2[n2].v, sizeof g2[0].v, "%s", _("not reported"));
            g2[n2].col = DIM;
        }
        n2++;

        g2[n2].k = _("Last full charge");
        if (sel_bat->energy_full_wh > 0) {
            snprintf(g2[n2].v, sizeof g2[0].v, "%.2f Wh", sel_bat->energy_full_wh);
            g2[n2].col = FG;
        } else {
            snprintf(g2[n2].v, sizeof g2[0].v, "%s", _("not reported"));
            g2[n2].col = DIM;
        }
        n2++;

        g2[n2].k = _("Design capacity");
        if (sel_bat->energy_design_wh > 0) {
            snprintf(g2[n2].v, sizeof g2[0].v, "%.2f Wh", sel_bat->energy_design_wh);
            g2[n2].col = FG;
        } else {
            snprintf(g2[n2].v, sizeof g2[0].v, "%s", _("not reported"));
            g2[n2].col = DIM;
        }
        n2++;

        /* Group 3: Environment, Thresholds & Handoff */
        g3[n3].k = _("Temperature");
        if (sel_bat->temp_c > -100.0) {
            snprintf(g3[n3].v, sizeof g3[0].v, "%.1f °C", sel_bat->temp_c);
            g3[n3].col = sel_bat->temp_c >= 45.0 ? POWER_WARN_COL : FG;
        } else {
            snprintf(g3[n3].v, sizeof g3[0].v, "%s", _("not reported"));
            g3[n3].col = DIM;
        }
        n3++;

        g3[n3].k = _("AC adapter");
        if (power_ac.online && power_ac.voltage_v > 0 && power_ac.current_a > 0) {
            snprintf(g3[n3].v, sizeof g3[0].v, _("Online (%.1f V, %.2f A)"),
                     power_ac.voltage_v, power_ac.current_a);
        } else {
            snprintf(g3[n3].v, sizeof g3[0].v, "%s", power_ac.online ? _("Online") : _("Offline"));
        }
        g3[n3++].col = power_ac.online ? POWER_GOOD_COL : FG;

        g3[n3].k = _("Charge limits");
        if (sel_bat->has_start_thresh || sel_bat->has_end_thresh) {
            snprintf(g3[n3].v, sizeof g3[0].v, _("start %d%%, stop %d%%"),
                     sel_bat->start_thresh, sel_bat->has_end_thresh ? sel_bat->end_thresh : 100);
            g3[n3].col = FG;
        } else if (rc_bat_start >= 0 || rc_bat_end >= 0) {
            snprintf(g3[n3].v, sizeof g3[0].v, _("saved %d%%–%d%% (applied on insert)"),
                     rc_bat_start >= 0 ? rc_bat_start : 0, rc_bat_end > 0 ? rc_bat_end : 100);
            g3[n3].col = DIM;
        } else {
            snprintf(g3[n3].v, sizeof g3[0].v, "%s", _("not exposed by firmware"));
            g3[n3].col = DIM;
        }
        n3++;

        g3[n3].k = _("Last handoff");
        if (npower_handoffs > 0) {
            power_format_handoff(&power_handoffs[npower_handoffs - 1], g3[n3].v, sizeof g3[0].v);
            g3[n3].col = POWER_HOFF_COL;
        } else {
            snprintf(g3[n3].v, sizeof g3[0].v, "%s", _("none in window"));
            g3[n3].col = DIM;
        }
        n3++;

        g3[n3].k = _("Pack ID");
        snprintf(g3[n3].v, sizeof g3[0].v, "%.44s", sel_bat->pack_id);
        g3[n3++].col = DIM;

        int right = cols >= 108 ? (n2 > n3 ? n2 : n3) : n2 + n3;
        int lines = n1 > right ? n1 : right;
        power_detail_max = lines > det_rows - 1 ? lines - (det_rows - 1) : 0;
        if (power_detail_scroll > power_detail_max) power_detail_scroll = power_detail_max;
        if (cols >= 108) {
            int cw3 = (cols - 6) / 3;
            int c0 = 2, c1 = 2 + cw3 + 1, c2 = 2 + 2 * (cw3 + 1);
            screen_text(c0, det_top, _("Battery & Health"), ACCENT, BG);
            screen_text(c1, det_top, _("Energy"), ACCENT, BG);
            screen_text(c2, det_top, _("Thresholds & Handoff"), ACCENT, BG);
            for (int r = 0; r < det_rows - 1; r++) {
                int idx = r + power_detail_scroll;
                char lbl[64];
                if (idx < n1) {
                    screen_text(c0, det_top + 1 + r, ui_pad(lbl, sizeof lbl, g1[idx].k, 17), DIM, BG);
                    screen_text(c0 + 18, det_top + 1 + r, g1[idx].v, g1[idx].col, BG);
                }
                if (idx < n2) {
                    screen_text(c1, det_top + 1 + r, ui_pad(lbl, sizeof lbl, g2[idx].k, 17), DIM, BG);
                    screen_text(c1 + 18, det_top + 1 + r, g2[idx].v, g2[idx].col, BG);
                }
                if (idx < n3) {
                    screen_text(c2, det_top + 1 + r, ui_pad(lbl, sizeof lbl, g3[idx].k, 16), DIM, BG);
                    screen_text(c2 + 17, det_top + 1 + r, g3[idx].v, g3[idx].col, BG);
                }
            }
        } else {
            int cw2 = (cols - 5) / 2;
            int c0 = 2, c1 = 2 + cw2 + 1;
            screen_text(c0, det_top, _("Battery & Health"), ACCENT, BG);
            screen_text(c1, det_top, _("Energy & Handoff"), ACCENT, BG);
            for (int r = 0; r < det_rows - 1; r++) {
                int idx = r + power_detail_scroll;
                char lbl[64];
                if (idx < n1) {
                    screen_text(c0, det_top + 1 + r, ui_pad(lbl, sizeof lbl, g1[idx].k, 16), DIM, BG);
                    screen_text(c0 + 17, det_top + 1 + r, g1[idx].v, g1[idx].col, BG);
                }
                if (idx < n2) {
                    screen_text(c1, det_top + 1 + r, ui_pad(lbl, sizeof lbl, g2[idx].k, 17), DIM, BG);
                    screen_text(c1 + 18, det_top + 1 + r, g2[idx].v, g2[idx].col, BG);
                } else if (idx - n2 < n3) {
                    int k = idx - n2;
                    screen_text(c1, det_top + 1 + r, ui_pad(lbl, sizeof lbl, g3[k].k, 17), DIM, BG);
                    screen_text(c1 + 18, det_top + 1 + r, g3[k].v, g3[k].col, BG);
                }
            }
        }
    }

    if (!power_detail_max) power_detail_scroll = 0;
    /* TRANSLATORS: m, Tab, Left/Right, r, Ctrl+C and Esc are key names; keep them. */
    screen_text(2, rows - 3, cols < 72 ? _("m: %/W  1-6: span  Arrows: cursor  r: refresh  Esc: back") :
                _("m: %/W   1-6: span   Left/Right: cursor   Tab: pack   r: refresh   Ctrl+C: copy   Esc: back"),
                DIM, BG);
    screen_text(2, rows - 2, _(notice), ACCENT, BG);
    screen_flush();
}

static int power_curves_pointer(const struct pointer_event *ev)
{
    if (ev->wheel) {
        int next = power_detail_scroll - ev->wheel;
        if (next < 0) next = 0;
        if (next > power_detail_max) next = power_detail_max;
        if (next == power_detail_scroll) return 0;
        power_detail_scroll = next;
        return 1;
    }
    for (int i = 0; i < npower_hits; i++) {
        const struct power_hit *h = &power_hits[i];
        if (ev->col < h->x || ev->col >= h->x + h->w ||
            ev->row < h->y || ev->row >= h->y + h->h) {
            continue;
        }
        if (h->kind == 6) {
            int nc;
            if (ev->button == BTN_RIGHT && ev->pressed) nc = -1;
            else if ((ev->button == BTN_LEFT && ev->pressed) || (!ev->button && (ev->buttons & 1)))
                nc = ev->col - h->x;
            else return 0;
            if (nc != power_cursor) {
                power_cursor = nc;
                return 1;
            }
            return 0;
        }
        if (ev->button == BTN_LEFT && ev->pressed) {
            if (h->kind == 1) {
                power_sel_dev = h->val;
                power_cursor = -1;
                power_load_selected_history();
            } else if (h->kind == 2) {
                power_mode = h->val;
            } else if (h->kind == 3) {
                power_span = h->val;
                power_load_selected_history();
            } else if (h->kind == 4) {
                power_refresh_devices();
                power_load_selected_history();
                snprintf(notice, sizeof notice, "%s", _("Battery readings refreshed."));
            } else if (h->kind == 5) {
                char rep[4096];
                power_build_clipboard_report(rep, sizeof rep);
                if (clipboard_set && clipboard_set(0, rep, strlen(rep)) == 0)
                    snprintf(notice, sizeof notice, "%s", _("Copied battery report to clipboard."));
                else
                    snprintf(notice, sizeof notice, "%s", _("Clipboard service is not running."));
            }
            return 1;
        }
    }
    return 0;
}

static void power_curves_page(void)
{
    power_refresh_devices();
    if (power_sel_dev >= npower_devs) power_sel_dev = 0;
    power_cursor = -1;
    power_detail_scroll = 0;
    notice[0] = '\0';
    power_load_selected_history();
    power_curves_draw();

    for (;;) {
        int key = key_poll_timeout(-1, 5000);
        if (key == 0 || key == K_SHOWN) {
            power_refresh_devices();
            power_load_selected_history();
            power_curves_draw();
            continue;
        }
        if (key == K_POINTER) {
            const struct pointer_event *e = pointer_last();
            if (settings_back_clicked(e)) key = 27;
            else {
                if (power_curves_pointer(e)) power_curves_draw();
                continue;
            }
        }
        int mods = mods_now(), act = key_list_action(key, mods);
        if (act == ACT_CANCEL || close_read(key)) { /* Esc, q: back; Ctrl+Q, the window's close: quit */
            if (!closing) notice[0] = '\0';
            return;
        }
        if (act == ACT_HELP) {
            help_screen();
            if (closing) return;
            power_curves_draw();
            continue;
        }
        if (act == ACT_COPY) {
            char rep[4096];
            power_build_clipboard_report(rep, sizeof rep);
            if (clipboard_set && clipboard_set(0, rep, strlen(rep)) == 0)
                snprintf(notice, sizeof notice, "%s", _("Copied battery report to clipboard."));
            else
                snprintf(notice, sizeof notice, "%s", _("Clipboard service is not running."));
            power_curves_draw();
            continue;
        } else if (act == ACT_NEXT_FIELD || act == ACT_PREV_FIELD) {
            key = '\t';
        } else if (act != ACT_NONE && act != ACT_DELETE) {
            continue; /* Delete is the cursor's: off */
        } else if (key >= ' ' && key < 0x7F && (mods & (KEYS_ALT | KEYS_META))) {
            continue; /* Alt+m is not m */
        }
        int cols = screen_cols();
        int plot_w = (cols - 4) - 10;
        if (plot_w < 10) plot_w = 10;

        if (key == 'm' || key == 'M') {
            power_mode = !power_mode;
        } else if (key >= '1' && key <= '6') {
            power_span = key - '1';
            power_load_selected_history();
        } else if (key == '-' || key == '[') {
            power_span = (power_span + NPSPANS - 1) % NPSPANS;
            power_load_selected_history();
        } else if (key == '+' || key == '=' || key == ']') {
            power_span = (power_span + 1) % NPSPANS;
            power_load_selected_history();
        } else if (key == '\t' && npower_devs > 1) { /* Tab, Shift+Tab: the next, the previous pack */
            power_sel_dev = (power_sel_dev + (act == ACT_PREV_FIELD ? npower_devs - 1 : 1)) % npower_devs;
            power_cursor = -1;
            power_load_selected_history();
        } else if (key == K_LEFT) {
            if (power_cursor < 0) power_cursor = plot_w - 1;
            else if (power_cursor > 0) power_cursor--;
            else continue;
        } else if (key == K_RIGHT) {
            if (power_cursor < 0) power_cursor = 0;
            else if (power_cursor + 1 < plot_w) power_cursor++;
            else continue;
        } else if (key == K_DEL || key == '0') {
            if (power_cursor < 0) continue;
            power_cursor = -1;
        } else if (key == K_UP && power_detail_scroll > 0) {
            power_detail_scroll--;
        } else if (key == K_DOWN && power_detail_scroll < power_detail_max) {
            power_detail_scroll++;
        } else if (key == 'r' || key == 'R') {
            power_refresh_devices();
            power_load_selected_history();
            snprintf(notice, sizeof notice, "%s", _("Battery readings refreshed."));
        } else {
            continue;
        }
        power_curves_draw();
    }
}
