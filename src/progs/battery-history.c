/*
 * battery-history: persistent battery history & handoff service (service_battery).
 *
 * Tracks physical battery packs by identity (vendor_model_serial) across
 * /sys/class/power_supply slots, detects hot-swaps on AC, cold-swaps across
 * hibernate/poweroff, and dual-battery discharge handoffs, re-applies saved
 * charge thresholds from rc.conf whenever a pack is inserted or swapped in,
 * and records 7-day rolling curves in /var/lib/locachy/battery/.
 *
 * Run "battery-history --once" to process transitions and record one snapshot.
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <linux/netlink.h>
#include <poll.h>
#include <signal.h>
#include <sys/file.h>
#include <sys/signalfd.h>
#include <sys/socket.h>

#include "battery-adaptive.h"

#ifndef DEFAULT_RC_CONF
#define DEFAULT_RC_CONF "/etc/locachy/rc.conf"
#endif

#define BAT_FLUSH_INTERVAL_SEC 600   /* flush buffered .tsv samples every 10 min */
#define BAT_STATIC_REFRESH_SEC 1800  /* re-read static EC attrs every 30 min */
#define BAT_BUF_MAX 16               /* pending RAM samples per tracked pack */
#define BAT_POLL_ACTIVE_MS 60000     /* 60 s while charging/discharging */
#define BAT_POLL_IDLE_AC_MS 300000   /* 5 min when AC online & all packs Full/Not charging */

static volatile sig_atomic_t stop_flag;
static volatile sig_atomic_t flush_flag;
static volatile sig_atomic_t reload_flag;

static void on_sig(int sig)
{
    if (sig == SIGUSR1) flush_flag = 1;
    else if (sig == SIGHUP) reload_flag = 1;
    else stop_flag = 1;
}

struct slot_state {
    char slot[48];
    int occupied;
    int ever_seen;
    struct bat_device dev;
    int64_t last_sample_ts;
    char last_state;
    int last_ac, last_ac_verified;
    int meta_written;
    struct bat_device saved_meta;
    struct bat_sample pending[BAT_BUF_MAX];
    int npending;
    int64_t last_flush_ts;
    int64_t charge_decided_at;
    struct bat_adaptive_decision charge_decision;
    struct bat_charge_slot_status charge_status;
};

static struct slot_state slots[BAT_MAX_DEVICES];
static int nslots;
static int any_prior_pack;
static char active_dis_slot[48];
static char active_dis_pack[96];
static int active_dis_dpct;

/* Daemon-only caches (only engaged by main()'s daemon loop; direct callers
 * such as test-battery-handoff.c invoking record_snapshot() get immediate
 * full scans & writes). */
static int daemon_mode;
static int64_t last_full_scan_ts;
static int last_ac_online;
static enum bat_system_ac last_system_ac;
static int all_packs_idle_ac;
static int64_t last_observation, last_monotonic, last_boot_time;

static int64_t current_epoch_ts(void)
{
    return bat_now_ts();
}

struct charge_config {
    int start, end, adaptive;
    int64_t full_until;
};

static struct charge_config charge_cfg = { .start = -1, .end = -1 };
static int64_t cancelled_full_until;
static int intent_loaded, intent_error;

static void read_charge_config(void)
{
    struct charge_config cfg = { .start = -1, .end = -1 };
    const char *rc_path = getenv("LOCACHY_RC_CONF");
    if (!rc_path || !rc_path[0]) rc_path = DEFAULT_RC_CONF;
    FILE *f = fopen(rc_path, "r");
    if (!f) {
        if (errno == ENOENT) charge_cfg = cfg;
        return;
    }
    char line[512];
    while (fgets(line, sizeof line, f)) {
        char *p = line;
        while (*p == ' ' || *p == '\t') p++;
        if (*p == '#' || *p == '\0') continue;
        char *eq = strchr(p, '=');
        if (!eq) continue;
        *eq++ = '\0';
        size_t kl = strlen(p);
        while (kl > 0 && (p[kl - 1] == ' ' || p[kl - 1] == '\t')) p[--kl] = '\0';
        while (*eq == ' ' || *eq == '\t') eq++;
        char *end;
        int quoted = *eq == '"';
        if (quoted) eq++;
        errno = 0;
        long long v = strtoll(eq, &end, 10);
        if (errno || end == eq || (quoted && *end++ != '"')) continue;
        while (*end == ' ' || *end == '\t') end++;
        if (*end != '\0' && *end != '\r' && *end != '\n' && *end != '#') continue;
        if (strcmp(p, "battery_charge_start") == 0 && v >= 0 && v <= 99) cfg.start = (int)v;
        else if (strcmp(p, "battery_charge_end") == 0 && v >= 20 && v <= 100) cfg.end = (int)v;
        else if (strcmp(p, "battery_charge_adaptive") == 0) cfg.adaptive = v == 1;
        else if (strcmp(p, "battery_charge_full_until") == 0 && v >= 0) cfg.full_until = v;
    }
    if (!ferror(f)) charge_cfg = cfg;
    fclose(f);
}

/* A baseline belongs to this pack in this slot, not to whichever pack is
 * inserted next. Keep it separate from historical .meta files, which are
 * rewritten for ordinary samples and are only keyed by pack. */
struct charge_baseline {
    int start, end;
    char start_attr[40], end_attr[40];
};

static int charge_baseline_path(const struct bat_device *b, char *path, size_t cap)
{
    if (!bat_safe_name(b->name) || !bat_safe_name(b->pack_id)) return -1;
    int n = snprintf(path, cap, "%s/.charge-%s.%s", bat_history_dir(), b->name, b->pack_id);
    return n > 0 && (size_t)n < cap ? 0 : -1;
}

static int charge_identity_matches(const struct bat_device *b)
{
    const char *root = bat_supply_root();
    int ok;
    long long present = bat_read_ll(root, b->name, "present", &ok);
    if (ok && present != 1) return 0;
    char vendor[64] = "", model[64] = "", serial[64] = "";
    (void)bat_read_line(root, b->name, "manufacturer", vendor, sizeof vendor);
    (void)bat_read_line(root, b->name, "model_name", model, sizeof model);
    (void)bat_read_line(root, b->name, "serial_number", serial, sizeof serial);
    return strcmp(vendor, b->vendor) == 0 && strcmp(model, b->model) == 0 &&
           strcmp(serial, b->serial) == 0;
}

/* 0 = not saved, 1 = valid saved baseline, -1 = unreadable/conflicting. */
static int load_charge_baseline(const struct bat_device *b, const char *path, struct charge_baseline *saved)
{
    FILE *f = fopen(path, "r");
    if (!f) return errno == ENOENT ? 0 : -1;
    char identity[3][66];
    int ok = 1;
    for (int i = 0; i < 3; i++) {
        if (!fgets(identity[i], sizeof identity[i], f) || !strchr(identity[i], '\n')) {
            ok = 0;
            break;
        }
        *strchr(identity[i], '\n') = '\0';
    }
    if (ok) {
        ok = strcmp(identity[0], b->vendor) == 0 && strcmp(identity[1], b->model) == 0 &&
             strcmp(identity[2], b->serial) == 0 &&
             fscanf(f, "%d %d %39s %39s", &saved->start, &saved->end,
                    saved->start_attr, saved->end_attr) == 4 &&
             saved->start >= -1 && saved->start < saved->end && saved->end > 0 && saved->end <= 100 &&
             strcmp(saved->end_attr, "charge_control_end_threshold") == 0 &&
             strcmp(saved->start_attr, saved->start < 0 ? "-" : "charge_control_start_threshold") == 0;
    }
    fclose(f);
    return ok ? 1 : -1;
}

static int sync_history_dir(void)
{
    int fd = open(bat_history_dir(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (fd < 0) return -1;
    int rc = fsync(fd);
    close(fd);
    return rc;
}

static int save_charge_baseline(const struct bat_device *b, const char *path, int start, int end)
{
    char tmp[600];
    int n = snprintf(tmp, sizeof tmp, "%s.tmp.XXXXXX", path);
    if (n <= 0 || (size_t)n >= sizeof tmp) return -1;
    int fd = mkstemp(tmp);
    if (fd < 0) return -1;
    int ok = dprintf(fd, "%s\n%s\n%s\n%d %d %s %s\n", b->vendor, b->model, b->serial,
                     start, end, start < 0 ? "-" : b->start_attr, b->end_attr) > 0 && fsync(fd) == 0;
    if (close(fd) != 0) ok = 0;
    if (!ok || rename(tmp, path) != 0) {
        unlink(tmp);
        return -1;
    }
    return sync_history_dir();
}

/* The writer never edits rc.conf. A consumed request cannot re-arm after
 * replug, a daemon restart, or a clock step back through its expiry. */
static int64_t charge_full_until(const struct bat_ac_info *ac, int64_t now)
{
    if (!charge_cfg.full_until) return 0;
    char path[560];
    int n = snprintf(path, sizeof path, "%s/charge-cancelled", bat_history_dir());
    if (n <= 0 || (size_t)n >= sizeof path) {
        intent_error = 1;
        return 0;
    }
    if (!intent_loaded || intent_error) {
        FILE *f = fopen(path, "r");
        if (!f) {
            intent_error = errno != ENOENT;
        } else {
            long long until;
            intent_error = fscanf(f, "%lld", &until) != 1 || until < 0;
            if (!intent_error && until > cancelled_full_until) cancelled_full_until = until;
            fclose(f);
        }
        intent_loaded = 1;
    }
    int64_t until = charge_cfg.full_until;
    if (intent_error || until <= cancelled_full_until) return 0;
    if (until > now && until - now <= BAT_CHARGE_FULL_SECONDS && ac->system == BAT_AC_ONLINE)
        return until;
    if (until - now > BAT_CHARGE_FULL_SECONDS) return 0;

    char tmp[600];
    snprintf(tmp, sizeof tmp, "%s.tmp.XXXXXX", path);
    int fd = mkstemp(tmp);
    int ok = fd >= 0;
    if (ok) {
        ok = dprintf(fd, "%lld\n", (long long)until) > 0 && fsync(fd) == 0;
        if (close(fd) != 0) ok = 0;
        if (!ok || rename(tmp, path) != 0) {
            unlink(tmp);
            ok = 0;
        }
        if (ok) ok = sync_history_dir() == 0;
    }
    if (ok) cancelled_full_until = until;
    intent_error = !ok;
    return 0;
}

static int present_charge_threshold(const struct bat_device *b, const char *attr)
{
    char path[512];
    int n = snprintf(path, sizeof path, "%s/%s/%s", bat_supply_root(), b->name, attr);
    struct stat st;
    if (n <= 0 || (size_t)n >= sizeof path) return -1;
    if (stat(path, &st) != 0) return errno == ENOENT ? 0 : -1;
    return S_ISREG(st.st_mode) ? 1 : -1;
}

/* Adaptive control only uses the kernel's standard power_supply ABI. */
static const char *charge_check_support(const struct bat_device *b)
{
    if (!b->serial[0] || !strcmp(b->serial, "0") || !strcasecmp(b->serial, "unknown"))
        return "unsupported";
    if (!b->has_end_thresh || strcmp(b->end_attr, "charge_control_end_threshold") != 0) {
        int present = present_charge_threshold(b, "charge_control_end_threshold");
        return present == 0 ? "unsupported" : "error";
    }
    if (b->has_start_thresh && strcmp(b->start_attr, "charge_control_start_threshold") != 0)
        return "unsupported";
    if (!b->has_start_thresh && present_charge_threshold(b, "charge_control_start_threshold") != 0)
        return "error";
    return NULL;
}

static struct slot_state *get_slot(const char *slot_name, int create)
{
    for (int i = 0; i < nslots; i++) {
        if (strcmp(slots[i].slot, slot_name) == 0) return &slots[i];
    }
    if (!create || nslots >= BAT_MAX_DEVICES) return NULL;
    struct slot_state *s = &slots[nslots++];
    memset(s, 0, sizeof *s);
    snprintf(s->slot, sizeof s->slot, "%s", slot_name);
    s->last_ac = -1;
    return s;
}

/* Restore previously inserted packs per slot from .meta files so cold-swaps
 * across hibernate/reboot are detected on startup. */
static void load_previous_slots(void)
{
    DIR *d = opendir(bat_history_dir());
    if (!d) return;
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        size_t len = strlen(e->d_name);
        if (len <= 5 || strcmp(e->d_name + len - 5, ".meta") != 0) continue;
        char mpath[560];
        int n = snprintf(mpath, sizeof mpath, "%s/%s", bat_history_dir(), e->d_name);
        if (n <= 0 || (size_t)n >= sizeof mpath) continue;
        struct bat_device b;
        int ins = 0;
        if (bat_load_pack_meta(mpath, &b, &ins) != 0 || !b.name[0]) continue;
        any_prior_pack = 1;
        struct slot_state *sl = get_slot(b.name, 1);
        if (sl) sl->ever_seen = 1;
        if (ins && sl && (!sl->occupied || b.last_seen_ts >= sl->dev.last_seen_ts)) {
            sl->occupied = 1;
            sl->dev = b;
            sl->dev.is_offline_pack = 0;
        }
    }
    closedir(d);
}

static int open_uevent_socket(void)
{
    int fd = socket(AF_NETLINK, SOCK_DGRAM | SOCK_CLOEXEC | SOCK_NONBLOCK,
                    NETLINK_KOBJECT_UEVENT);
    if (fd < 0) return -1;
    int rcv = 256 * 1024;
    (void)setsockopt(fd, SOL_SOCKET, SO_RCVBUFFORCE, &rcv, sizeof rcv);
    (void)setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &rcv, sizeof rcv);
    struct sockaddr_nl sa;
    memset(&sa, 0, sizeof sa);
    sa.nl_family = AF_NETLINK;
    sa.nl_groups = 1;
    if (bind(fd, (struct sockaddr *)&sa, sizeof sa) != 0) {
        close(fd);
        return -1;
    }
    return fd;
}

static int drain_power_uevents(int fd)
{
    char buf[4096];
    int power = 0;
    for (;;) {
        ssize_t n = recv(fd, buf, sizeof buf - 1, 0);
        if (n < 0) {
            if (errno == EINTR) continue;
            if (errno == ENOBUFS) {
                power = 1;
                continue;
            }
            if (errno == EAGAIN || errno == EWOULDBLOCK) break;
            return -1;
        }
        if (n == 0) return -1;
        buf[n] = '\0';
        for (ssize_t i = 0; i < n; ) {
            const char *s = buf + i;
            if (strcmp(s, "SUBSYSTEM=power_supply") == 0) power = 1;
            i += (ssize_t)strlen(s) + 1;
        }
    }
    return power;
}

static int mwh_of(double wh)
{
    return wh > 0 ? (int)(wh * 1000.0 + 0.5) : -1;
}

static int mv_of(double v)
{
    return v > 0 ? (int)(v * 1000.0 + 0.5) : -1;
}

static int pack_meta_changed(const struct slot_state *sl, const struct bat_device *b)
{
    if (!sl->meta_written) return 1;
    const struct bat_device *m = &sl->saved_meta;
    return strcmp(m->pack_id, b->pack_id) != 0 ||
           strcmp(m->name, b->name) != 0 ||
           strcmp(m->vendor, b->vendor) != 0 ||
           strcmp(m->model, b->model) != 0 ||
           strcmp(m->serial, b->serial) != 0 ||
           strcmp(m->technology, b->technology) != 0 ||
           mwh_of(m->energy_full_wh) != mwh_of(b->energy_full_wh) ||
           mwh_of(m->energy_design_wh) != mwh_of(b->energy_design_wh) ||
           mv_of(m->voltage_design_v) != mv_of(b->voltage_design_v) ||
           m->cycles != b->cycles ||
           m->has_start_thresh != b->has_start_thresh ||
           m->has_end_thresh != b->has_end_thresh ||
           m->start_thresh != b->start_thresh ||
           m->end_thresh != b->end_thresh;
}

static void save_slot_meta(struct slot_state *sl, int inserted, int64_t ts)
{
    if (bat_save_pack_meta(&sl->dev, inserted, ts) == 0) {
        sl->meta_written = inserted ? 1 : 0;
        sl->saved_meta = sl->dev;
    }
}

static void flush_slot_samples(struct slot_state *sl, int64_t now_ts)
{
    if (sl->npending <= 0) return;
    if (bat_append_samples_batch(sl->dev.pack_id, sl->pending, sl->npending) == 0) {
        sl->npending = 0;
        sl->last_flush_ts = now_ts;
    }
}

static void flush_all_pending(int save_meta, int64_t now_ts)
{
    for (int j = 0; j < nslots; j++) {
        struct slot_state *sl = &slots[j];
        if (!sl->occupied) continue;
        flush_slot_samples(sl, now_ts);
        if (save_meta && now_ts >= 946684800LL) {
            save_slot_meta(sl, 1, sl->dev.last_seen_ts > 0 ? sl->dev.last_seen_ts : now_ts);
        }
    }
}

static void queue_slot_sample(struct slot_state *sl, const struct bat_sample *s, int immediate)
{
    if (!daemon_mode || immediate) {
        flush_slot_samples(sl, s->ts);
        if (bat_append_sample(sl->dev.pack_id, s) == 0) {
            sl->last_sample_ts = s->ts;
            sl->last_state = s->state;
            sl->last_ac = s->ac;
            sl->last_ac_verified = s->ac_verified;
            sl->last_flush_ts = s->ts;
        }
        return;
    }
    if (sl->npending >= BAT_BUF_MAX) {
        flush_slot_samples(sl, s->ts);
    }
    if (sl->npending < BAT_BUF_MAX) {
        sl->pending[sl->npending++] = *s;
        sl->last_sample_ts = s->ts;
        sl->last_state = s->state;
        sl->last_ac = s->ac;
        sl->last_ac_verified = s->ac_verified;
        if (sl->last_flush_ts == 0) sl->last_flush_ts = s->ts;
        if (sl->npending >= BAT_BUF_MAX || s->ts - sl->last_flush_ts >= BAT_FLUSH_INTERVAL_SEC) {
            flush_slot_samples(sl, s->ts);
        }
    } else if (bat_append_sample(sl->dev.pack_id, s) == 0) {
        sl->last_sample_ts = s->ts;
        sl->last_state = s->state;
        sl->last_ac = s->ac;
        sl->last_ac_verified = s->ac_verified;
        sl->last_flush_ts = s->ts;
    }
}

static void clear_charge_status(void)
{
    char path[560];
    int n = snprintf(path, sizeof path, "%s/charge-status", bat_history_dir());
    if (n > 0 && (size_t)n < sizeof path) (void)unlink(path);
}

static void publish_charge_status(const struct bat_charge_status *status)
{
    struct bat_charge_status previous;
    if (bat_read_charge_status(&previous) == 0 && previous.adaptive == status->adaptive &&
        previous.full_until == status->full_until && previous.nslots == status->nslots) {
        int same = 1;
        for (int i = 0; i < status->nslots; i++) {
            const struct bat_charge_slot_status *a = &previous.slots[i], *b = &status->slots[i];
            if (strcmp(a->slot, b->slot) || strcmp(a->pack_id, b->pack_id) || strcmp(a->state, b->state) ||
                a->requested_end != b->requested_end || a->actual_end != b->actual_end ||
                a->release_ts != b->release_ts) {
                same = 0;
                break;
            }
        }
        if (same) return;
    }
    char path[560], tmp[600];
    int n = snprintf(path, sizeof path, "%s/charge-status", bat_history_dir());
    if (n <= 0 || (size_t)n >= sizeof path) return;
    snprintf(tmp, sizeof tmp, "%s.tmp.XXXXXX", path);
    int fd = mkstemp(tmp);
    if (fd < 0) return;
    int ok = fchmod(fd, 0644) == 0 && dprintf(fd, "%d %lld %d\n", status->adaptive,
                                             (long long)status->full_until, status->nslots) > 0;
    for (int i = 0; ok && i < status->nslots; i++) {
        const struct bat_charge_slot_status *s = &status->slots[i];
        ok = dprintf(fd, "%s %s %s %d %d %lld\n", s->slot, s->pack_id, s->state,
                     s->requested_end, s->actual_end, (long long)s->release_ts) > 0;
    }
    if (close(fd) != 0) ok = 0;
    if (!ok || rename(tmp, path) != 0) unlink(tmp);
}

static void charge_sample(const struct bat_device *b, const struct bat_ac_info *ac,
                          int64_t now, struct bat_sample *s)
{
    bat_sample_from_device(b, ac->system == BAT_AC_UNKNOWN ? ac->online :
                           ac->system == BAT_AC_ONLINE, now, s);
    s->ac_verified = ac->system != BAT_AC_UNKNOWN;
}

static struct bat_adaptive_decision charge_decide(const struct slot_state *sl, int64_t now,
                                                  const struct bat_ac_info *ac)
{
    static struct bat_sample samples[BAT_MAX_SAMPLES + BAT_BUF_MAX + 1];
    int n = bat_load_samples(sl->dev.pack_id, NULL, now - BAT_ADAPTIVE_WINDOW,
                            samples, BAT_MAX_SAMPLES, 1);
    for (int i = 0; i < sl->npending; i++) samples[n++] = sl->pending[i];
    charge_sample(&sl->dev, ac, now, &samples[n++]);
    return bat_adaptive_decide(samples, (size_t)n, now);
}

static void update_charge_slot(struct slot_state *sl, const struct bat_ac_info *ac, int64_t now,
                               int64_t full_until, int manual, int evaluate, int single_pack)
{
    struct bat_device *b = &sl->dev;
    struct bat_charge_slot_status *s = &sl->charge_status;
    memset(s, 0, sizeof *s);
    snprintf(s->slot, sizeof s->slot, "%s", b->name);
    snprintf(s->pack_id, sizeof s->pack_id, "%s", b->pack_id);
    s->requested_end = -1;
    const char *state = "manual";
    int managed = charge_cfg.adaptive || full_until > 0;
    char path[560];
    struct charge_baseline saved;
    int have = charge_baseline_path(b, path, sizeof path) == 0 ? load_charge_baseline(b, path, &saved) : -1;
    if (have < 0) {
        state = "error";
        goto done;
    }
    const char *unsupported = managed ? (single_pack ? charge_check_support(b) : "unsupported") : NULL;
    if (managed && !unsupported && ac->system == BAT_AC_ONLINE) {
        int start, end;
        if (full_until) {
            state = "full";
            start = 99;
            end = 100;
        } else {
            static const char *const labels[] = {
                [BAT_ADAPTIVE_LEARNING] = "learning", [BAT_ADAPTIVE_MAINS] = "mains",
                [BAT_ADAPTIVE_MOBILITY] = "mobility", [BAT_ADAPTIVE_HOLD] = "hold",
                [BAT_ADAPTIVE_TOP_UP] = "top-up",
            };
            if (evaluate || !sl->charge_decided_at || now < sl->charge_decided_at ||
                now - sl->charge_decided_at >= BAT_POLL_IDLE_AC_MS / 1000 ||
                (sl->charge_decision.release_ts > sl->charge_decided_at && now >= sl->charge_decision.release_ts)) {
                sl->charge_decision = charge_decide(sl, now, ac);
                sl->charge_decided_at = now;
            }
            const struct bat_adaptive_decision *d = &sl->charge_decision;
            state = labels[d->state];
            start = d->start_pct;
            end = d->end_pct;
            s->release_ts = d->release_ts;
        }
        if (end < 0) goto manual_limits;
        s->requested_end = end;
        if (!b->has_start_thresh) start = -1;
        if ((b->has_start_thresh && bat_read_threshold(b, b->start_attr, 0, &b->start_thresh) != 0) ||
            bat_read_threshold(b, b->end_attr, 1, &b->end_thresh) != 0 ||
            (b->has_start_thresh && b->start_thresh >= b->end_thresh)) {
            state = "error";
            goto done;
        }
        if ((b->has_start_thresh && b->start_thresh != start) || b->end_thresh != end) {
            if (!charge_identity_matches(b) ||
                (have && (strcmp(saved.end_attr, b->end_attr) ||
                           strcmp(saved.start_attr, b->has_start_thresh ? b->start_attr : "-"))) ||
                (!have && save_charge_baseline(b, path, b->has_start_thresh ? b->start_thresh : -1,
                                               b->end_thresh) != 0) ||
                bat_apply_thresholds_to(b, start, end) != 0) state = "error";
        }
        goto done;
    }
    sl->charge_decided_at = 0;
    if (unsupported) state = unsupported;
    else if (charge_cfg.adaptive) state = ac->system == BAT_AC_OFFLINE ? "on-battery" : "learning";

manual_limits:
    if (have) {
        int start = charge_cfg.start >= 0 ? charge_cfg.start : saved.start;
        int end = charge_cfg.end >= 0 ? charge_cfg.end : saved.end;
        if (!b->has_end_thresh || strcmp(saved.end_attr, b->end_attr) ||
            strcmp(saved.start_attr, b->has_start_thresh ? b->start_attr : "-") ||
            !charge_identity_matches(b) || bat_apply_thresholds_to(b, start, end) != 0 ||
            unlink(path) != 0 || sync_history_dir() != 0) {
            state = "error";
            goto done;
        }
        if (b->has_end_thresh) s->requested_end = charge_cfg.end;
    } else if (manual && (charge_cfg.start >= 0 || charge_cfg.end >= 0)) {
        if (b->has_end_thresh) s->requested_end = charge_cfg.end;
        if (!charge_identity_matches(b) || bat_apply_thresholds_to(b, charge_cfg.start, charge_cfg.end) != 0)
            state = "error";
    }
done:
    if (intent_error && charge_cfg.full_until) state = "error";
    s->actual_end = b->has_end_thresh ? b->end_thresh : -1;
    snprintf(s->state, sizeof s->state, "%s", state);
}

static int scan_for_snapshot(int full_scan, int64_t now_ts, struct bat_device *live, struct bat_ac_info *ac)
{
    int have_occupied = 0;
    for (int j = 0; j < nslots; j++) {
        if (slots[j].occupied) have_occupied++;
    }
    if (full_scan || !daemon_mode || !have_occupied || last_full_scan_ts == 0 ||
        now_ts - last_full_scan_ts >= BAT_STATIC_REFRESH_SEC) {
        int nlive = bat_scan_live(live, BAT_MAX_DEVICES, ac);
        last_full_scan_ts = now_ts;
        return nlive;
    }
    /* Routine timer tick with no power_supply uevent: read only dynamic
     * attributes for tracked slots plus AC online state. Fall back to a full
     * scan immediately if any slot vanished or fails dynamic refresh. */
    if (bat_scan_ac_only(ac) != 0) {
        int nlive = bat_scan_live(live, BAT_MAX_DEVICES, ac);
        last_full_scan_ts = now_ts;
        return nlive;
    }
    int nlive = 0;
    for (int j = 0; j < nslots; j++) {
        if (!slots[j].occupied) continue;
        struct bat_device d = slots[j].dev;
        if (bat_refresh_dynamic(&d, now_ts) != 0) {
            nlive = bat_scan_live(live, BAT_MAX_DEVICES, ac);
            last_full_scan_ts = now_ts;
            return nlive;
        }
        live[nlive++] = d;
    }
    if (!ac->any_adapter && nlive > 0) {
        for (int i = 0; i < nlive; i++) {
            if (!live[i].is_peripheral &&
                (strcmp(live[i].status, "Charging") == 0 || strcmp(live[i].status, "Full") == 0)) {
                ac->online = 1;
            }
        }
    }
    return nlive;
}

static int record_snapshot_ex(int force, int full_scan, int reapply_manual)
{
    struct bat_device live[BAT_MAX_DEVICES];
    struct bat_ac_info ac;
    int64_t now_ts = current_epoch_ts();
    /* A short sleep or wall-clock adjustment can fit between normal samples.
     * Mark the boundary without scheduling any extra timer. */
    int continuity_break = 0;
    if (daemon_mode) {
        struct timespec monotonic, boot;
        if (clock_gettime(CLOCK_MONOTONIC, &monotonic) == 0 &&
            clock_gettime(CLOCK_BOOTTIME, &boot) == 0) {
            int64_t elapsed = monotonic.tv_sec - last_monotonic;
            int64_t slept = boot.tv_sec - last_boot_time - elapsed;
            int64_t clock_step = now_ts - last_observation - elapsed;
            continuity_break = !last_observation || slept > 2 ||
                               clock_step > 2 || clock_step < -2;
            last_observation = now_ts;
            last_monotonic = monotonic.tv_sec;
            last_boot_time = boot.tv_sec;
        }
    }
    int nlive = scan_for_snapshot(full_scan, now_ts, live, &ac);
    int manual_slots[BAT_MAX_DEVICES] = { 0 };
    int slot_seen[BAT_MAX_DEVICES] = { 0 };
    int sys_bats = 0;
    int force_all_samples = force;
    int transition = 0;
    int idle_ac = (ac.system == BAT_AC_ONLINE);

    /* 1. Match live system batteries against tracked slots (detect INSERT & SWAP) */
    for (int i = 0; i < nlive; i++) {
        if (live[i].is_peripheral) continue;
        sys_bats++;
        if (strcmp(live[i].status, "Full") != 0 && strcmp(live[i].status, "Not charging") != 0) {
            idle_ac = 0;
        }
        live[i].last_seen_ts = now_ts;
        struct slot_state *sl = get_slot(live[i].name, 1);
        if (!sl) continue;
        int sl_idx = (int)(sl - slots);
        slot_seen[sl_idx] = 1;

        if (!sl->occupied) {
            /* Newly inserted pack (emit INSERT unless this is initial cold discovery) */
            if (sl->ever_seen || any_prior_pack) {
                bat_append_handoff("INSERT", now_ts, sl->slot,
                                   "-", ac.online, live[i].pack_id, live[i].capacity_decipct);
            }
            manual_slots[sl_idx] = 1;
            sl->occupied = 1;
            sl->ever_seen = 1;
            any_prior_pack = 1;
            sl->dev = live[i];
            sl->last_sample_ts = 0;
            sl->npending = 0;
            sl->meta_written = 0;
            force_all_samples = 1;
            transition = 1;
        } else if (strcmp(sl->dev.pack_id, live[i].pack_id) != 0) {
            /* Physical battery pack swapped in the same slot (hot-swap or hibernate cold-swap)! */
            flush_slot_samples(sl, now_ts);
            int64_t rem_ts = (sl->dev.last_seen_ts > 0 && sl->dev.last_seen_ts < now_ts)
                             ? sl->dev.last_seen_ts + 1 : now_ts;
            struct bat_sample rem = {
                .ts = rem_ts,
                .decipct = sl->dev.capacity_decipct,
                .power_mw = 0,
                .energy_mwh = sl->dev.energy_now_wh >= 0 ? (int)(sl->dev.energy_now_wh * 1000.0 + 0.5) : 0,
                .voltage_mv = 0,
                .state = 'R',
                .ac = ac.online ? 1 : 0,
            };
            bat_append_sample(sl->dev.pack_id, &rem);
            save_slot_meta(sl, 0, sl->dev.last_seen_ts > 0 ? sl->dev.last_seen_ts : now_ts);

            bat_append_handoff("SWAP", now_ts, sl->slot,
                               sl->dev.pack_id, sl->dev.capacity_decipct,
                               live[i].pack_id, live[i].capacity_decipct);
            manual_slots[sl_idx] = 1;

            sl->dev = live[i];
            sl->last_sample_ts = 0;
            sl->npending = 0;
            sl->meta_written = 0;
            force_all_samples = 1;
            transition = 1;
        } else {
            sl->dev = live[i];
        }
    }

    /* 2. Detect slots whose battery pack was removed (hot-unplug on AC or removed while off) */
    for (int j = 0; j < nslots; j++) {
        if (!slots[j].occupied || slot_seen[j]) continue;
        struct slot_state *sl = &slots[j];
        flush_slot_samples(sl, now_ts);
        int64_t rem_ts = now_ts;
        struct bat_sample rem = {
            .ts = rem_ts,
            .decipct = sl->dev.capacity_decipct,
            .power_mw = 0,
            .energy_mwh = sl->dev.energy_now_wh >= 0 ? (int)(sl->dev.energy_now_wh * 1000.0 + 0.5) : 0,
            .voltage_mv = 0,
            .state = 'R',
            .ac = ac.online ? 1 : 0,
        };
        bat_append_sample(sl->dev.pack_id, &rem);
        save_slot_meta(sl, 0, sl->dev.last_seen_ts > 0 ? sl->dev.last_seen_ts : now_ts);
        bat_append_handoff("REMOVE", now_ts, sl->slot,
                           sl->dev.pack_id, sl->dev.capacity_decipct, "-", ac.online);
        if (strcmp(active_dis_slot, sl->slot) == 0) {
            active_dis_slot[0] = '\0';
            active_dis_pack[0] = '\0';
        }
        sl->occupied = 0;
        sl->last_sample_ts = 0;
        sl->npending = 0;
        force_all_samples = 1;
        transition = 1;
    }

    if (reapply_manual || transition || !daemon_mode) read_charge_config();
    struct bat_charge_status charge = {
        .adaptive = charge_cfg.adaptive,
        .full_until = charge_full_until(&ac, now_ts),
    };
    int full_applied = 0;
    for (int j = 0; j < nslots; j++) {
        struct slot_state *sl = &slots[j];
        if (!sl->occupied) continue;
        if (continuity_break) {
            struct bat_sample gap;
            charge_sample(&sl->dev, &ac, now_ts, &gap);
            gap.ac_verified = 0;
            queue_slot_sample(sl, &gap, 1);
        }
        update_charge_slot(sl, &ac, now_ts, charge.full_until, reapply_manual || manual_slots[j],
                           reapply_manual || transition || continuity_break || ac.system != last_system_ac,
                           sys_bats == 1);
        if (!strcmp(sl->charge_status.state, "full")) full_applied = 1;
        charge.slots[charge.nslots++] = sl->charge_status;
    }
    if (!full_applied) charge.full_until = 0;
    publish_charge_status(&charge);

    /* 3. Detect dual-battery active discharge handoff (e.g. BAT1 empty -> BAT0 discharging) */
    if (!ac.online) {
        const struct bat_device *cur_dis = NULL;
        for (int i = 0; i < nlive; i++) {
            if (!live[i].is_peripheral && strcmp(live[i].status, "Discharging") == 0) {
                cur_dis = &live[i];
                break;
            }
        }
        if (cur_dis) {
            if (active_dis_slot[0] && strcmp(active_dis_slot, cur_dis->name) != 0) {
                int old_dp = active_dis_dpct;
                for (int i = 0; i < nlive; i++) {
                    if (strcmp(live[i].name, active_dis_slot) == 0) {
                        old_dp = live[i].capacity_decipct;
                        break;
                    }
                }
                char pair[96];
                snprintf(pair, sizeof pair, "%s->%s", active_dis_slot, cur_dis->name);
                bat_append_handoff("DISCHARGE", now_ts, pair,
                                   active_dis_pack, old_dp,
                                   cur_dis->pack_id, cur_dis->capacity_decipct);
                force_all_samples = 1;
                transition = 1;
            }
            snprintf(active_dis_slot, sizeof active_dis_slot, "%s", cur_dis->name);
            snprintf(active_dis_pack, sizeof active_dis_pack, "%s", cur_dis->pack_id);
            active_dis_dpct = cur_dis->capacity_decipct;
        }
    } else {
        active_dis_slot[0] = '\0';
        active_dis_pack[0] = '\0';
    }

    /* 4. Buffer/append curve samples & write pack metadata only on change */
    if (now_ts >= 946684800LL) {
        for (int j = 0; j < nslots; j++) {
            if (!slots[j].occupied) continue;
            struct slot_state *sl = &slots[j];
            struct bat_sample s;
            charge_sample(&sl->dev, &ac, now_ts, &s);
            int changed = sl->last_sample_ts == 0 || sl->last_state != s.state ||
                          sl->last_ac != s.ac || sl->last_ac_verified != s.ac_verified;
            int64_t dt = now_ts - sl->last_sample_ts;
            if (!force_all_samples && !changed && dt >= 0 && dt < 55) continue;
            int immediate = force_all_samples || changed || transition;
            queue_slot_sample(sl, &s, immediate);
            if (pack_meta_changed(sl, &sl->dev)) {
                save_slot_meta(sl, 1, now_ts);
            }
        }
    }
    last_ac_online = ac.online;
    last_system_ac = ac.system;
    all_packs_idle_ac = (sys_bats > 0 && idle_ac);
    return sys_bats;
}

static int record_snapshot(int force)
{
    return record_snapshot_ex(force, 1, force);
}

int main(int argc, char **argv)
{
    int once = argc > 1 && strcmp(argv[1], "--once") == 0;
    if (!getenv("LOCACHY_BATTERY_HISTORY_DIR")) (void)mkdir("/var/lib/locachy", 0755);
    (void)mkdir(bat_history_dir(), 0755);
    char lockpath[560];
    int n = snprintf(lockpath, sizeof lockpath, "%s/.charge.lock", bat_history_dir());
    int lockfd = n > 0 && (size_t)n < sizeof lockpath ?
                 open(lockpath, O_CREAT | O_RDWR | O_CLOEXEC | O_NOFOLLOW, 0600) : -1;
    if (lockfd < 0 || flock(lockfd, LOCK_EX | LOCK_NB) != 0) {
        fprintf(stderr, "battery-history: cannot acquire charge writer lock\n");
        return 1;
    }
    clear_charge_status();
    load_previous_slots();
    if (once) {
        record_snapshot(1);
        clear_charge_status();
        close(lockfd);
        return 0;
    }

    daemon_mode = 1;

    sigset_t sigmask, oldmask;
    sigemptyset(&sigmask);
    sigaddset(&sigmask, SIGTERM);
    sigaddset(&sigmask, SIGINT);
    sigaddset(&sigmask, SIGUSR1);
    sigaddset(&sigmask, SIGHUP);
    int sfd = -1;
    if (sigprocmask(SIG_BLOCK, &sigmask, &oldmask) == 0) {
        sfd = signalfd(-1, &sigmask, SFD_NONBLOCK | SFD_CLOEXEC);
        if (sfd < 0) sigprocmask(SIG_SETMASK, &oldmask, NULL);
    }
    if (sfd < 0) {
        struct sigaction sa;
        memset(&sa, 0, sizeof sa);
        sa.sa_handler = on_sig;
        sigaction(SIGTERM, &sa, NULL);
        sigaction(SIGINT, &sa, NULL);
        sigaction(SIGUSR1, &sa, NULL);
        sigaction(SIGHUP, &sa, NULL);
    }
    signal(SIGPIPE, SIG_IGN);

    int ufd = open_uevent_socket();
    int sys_bats = record_snapshot_ex(1, 1, 1);

    while (!stop_flag) {
        struct pollfd pfd[2] = {
            { .fd = ufd, .events = POLLIN },
            { .fd = sfd, .events = POLLIN },
        };
        int timeout_ms;
        if (sys_bats <= 0) {
            timeout_ms = (ufd >= 0 && sfd >= 0) ? -1 : BAT_POLL_IDLE_AC_MS;
        } else if (all_packs_idle_ac && last_ac_online) {
            timeout_ms = BAT_POLL_IDLE_AC_MS;
        } else {
            timeout_ms = BAT_POLL_ACTIVE_MS;
        }
        int r = poll(pfd, 2, timeout_ms);
        if (r > 0 && sfd >= 0) {
            if (pfd[1].revents & (POLLERR | POLLHUP | POLLNVAL)) {
                close(sfd);
                sfd = -1;
                struct sigaction sa;
                memset(&sa, 0, sizeof sa);
                sa.sa_handler = on_sig;
                sigaction(SIGTERM, &sa, NULL);
                sigaction(SIGINT, &sa, NULL);
                sigaction(SIGUSR1, &sa, NULL);
                sigaction(SIGHUP, &sa, NULL);
                sigprocmask(SIG_SETMASK, &oldmask, NULL);
            } else if (pfd[1].revents & POLLIN) {
                struct signalfd_siginfo si;
                for (;;) {
                    ssize_t nr = read(sfd, &si, sizeof si);
                    if (nr != (ssize_t)sizeof si) {
                        if (nr < 0 && errno == EINTR) continue;
                        break;
                    }
                    if (si.ssi_signo == SIGUSR1) flush_flag = 1;
                    else if (si.ssi_signo == SIGHUP) reload_flag = 1;
                    else stop_flag = 1;
                }
            }
        }
        if (stop_flag) break;
        int need_snapshot = 0;
        int full_scan = 0;
        if (r == 0) {
            need_snapshot = 1;
        } else if (r > 0 && ufd >= 0) {
            if (pfd[0].revents & (POLLIN | POLLERR)) {
                int uev = drain_power_uevents(ufd);
                if (uev < 0) {
                    close(ufd);
                    ufd = -1;
                    full_scan = 1;
                    need_snapshot = 1;
                } else if (uev > 0 || (pfd[0].revents & POLLERR)) {
                    if (uev > 0) {
                        /* Give EC 150 ms to settle status/serial/thresholds after pack hot-swap or AC plug. */
                        struct timespec ts = { .tv_sec = 0, .tv_nsec = 150000000L };
                        nanosleep(&ts, NULL);
                    }
                    full_scan = 1;
                    need_snapshot = 1;
                }
            } else if (pfd[0].revents & (POLLHUP | POLLNVAL)) {
                close(ufd);
                ufd = -1;
                full_scan = 1;
                need_snapshot = 1;
            }
        }
        if (reload_flag) {
            reload_flag = 0;
            sys_bats = record_snapshot_ex(0, 1, 1);
            flush_all_pending(1, current_epoch_ts());
        } else if (need_snapshot) {
            sys_bats = record_snapshot_ex(0, full_scan, 0);
        }
        if (flush_flag) {
            flush_flag = 0;
            flush_all_pending(1, current_epoch_ts());
        }
        if (r < 0 && errno != EINTR) {
            struct timespec ts = { .tv_sec = 0, .tv_nsec = 100000000L };
            nanosleep(&ts, NULL);
        }
    }
    flush_all_pending(1, current_epoch_ts());
    clear_charge_status();
    if (sfd >= 0) {
        close(sfd);
        sigprocmask(SIG_SETMASK, &oldmask, NULL);
    }
    if (lockfd >= 0) close(lockfd);
    return 0;
}
