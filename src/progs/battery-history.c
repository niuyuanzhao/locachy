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
#include <sys/signalfd.h>
#include <sys/socket.h>

#include "battery-history.h"

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
    int last_ac;
    int meta_written;
    struct bat_device saved_meta;
    struct bat_sample pending[BAT_BUF_MAX];
    int npending;
    int64_t last_flush_ts;
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
static int all_packs_idle_ac;

static int64_t current_epoch_ts(void)
{
    return bat_now_ts();
}

static void read_rc_thresholds(int *start_out, int *end_out)
{
    *start_out = -1;
    *end_out = -1;
    const char *rc_path = getenv("LOCACHY_RC_CONF");
    if (!rc_path || !rc_path[0]) rc_path = DEFAULT_RC_CONF;
    FILE *f = fopen(rc_path, "r");
    if (!f) return;
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
        while (*eq == ' ' || *eq == '\t' || *eq == '"') eq++;
        eq[strcspn(eq, "\"\r\n")] = '\0';
        if (strcmp(p, "battery_charge_start") == 0 && eq[0]) {
            int v = atoi(eq);
            if (v >= 0 && v <= 99) *start_out = v;
        } else if (strcmp(p, "battery_charge_end") == 0 && eq[0]) {
            int v = atoi(eq);
            if (v >= 20 && v <= 100) *end_out = v;
        }
    }
    fclose(f);
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
        if (sl->last_flush_ts == 0) sl->last_flush_ts = s->ts;
        if (sl->npending >= BAT_BUF_MAX || s->ts - sl->last_flush_ts >= BAT_FLUSH_INTERVAL_SEC) {
            flush_slot_samples(sl, s->ts);
        }
    } else if (bat_append_sample(sl->dev.pack_id, s) == 0) {
        sl->last_sample_ts = s->ts;
        sl->last_state = s->state;
        sl->last_ac = s->ac;
        sl->last_flush_ts = s->ts;
    }
}

static void ensure_rc_thresholds(int *start_out, int *end_out)
{
    read_rc_thresholds(start_out, end_out);
}

static void reapply_rc_thresholds(void)
{
    int rc_start = -1, rc_end = -1;
    ensure_rc_thresholds(&rc_start, &rc_end);
    if (rc_start < 0 && rc_end < 0) return;
    for (int j = 0; j < nslots; j++) {
        if (!slots[j].occupied) continue;
        (void)bat_apply_thresholds_to(&slots[j].dev, rc_start, rc_end);
    }
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

static int record_snapshot_ex(int force, int full_scan)
{
    struct bat_device live[BAT_MAX_DEVICES];
    struct bat_ac_info ac;
    int64_t now_ts = current_epoch_ts();
    int nlive = scan_for_snapshot(full_scan, now_ts, live, &ac);
    int rc_start = -1, rc_end = -1;
    int rc_fetched = 0;

    int slot_seen[BAT_MAX_DEVICES] = { 0 };
    int sys_bats = 0;
    int force_all_samples = force;
    int transition = 0;
    int idle_ac = ac.online ? 1 : 0;

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
            if (!rc_fetched) {
                ensure_rc_thresholds(&rc_start, &rc_end);
                rc_fetched = 1;
            }
            /* Newly inserted pack (emit INSERT unless this is initial cold discovery) */
            if (sl->ever_seen || any_prior_pack) {
                bat_append_handoff("INSERT", now_ts, sl->slot,
                                   "-", ac.online, live[i].pack_id, live[i].capacity_decipct);
            }
            if (rc_start >= 0 || rc_end >= 0)
                (void)bat_apply_thresholds_to(&live[i], rc_start, rc_end);
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
            if (!rc_fetched) {
                ensure_rc_thresholds(&rc_start, &rc_end);
                rc_fetched = 1;
            }
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
            if (rc_start >= 0 || rc_end >= 0)
                (void)bat_apply_thresholds_to(&live[i], rc_start, rc_end);

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
            bat_sample_from_device(&sl->dev, ac.online, now_ts, &s);
            int changed = (sl->last_sample_ts == 0 || sl->last_state != s.state || sl->last_ac != s.ac);
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
    all_packs_idle_ac = (sys_bats > 0 && idle_ac);
    return sys_bats;
}

static int record_snapshot(int force)
{
    return record_snapshot_ex(force, 1);
}

int main(int argc, char **argv)
{
    int once = argc > 1 && strcmp(argv[1], "--once") == 0;
    load_previous_slots();
    if (once) {
        record_snapshot(1);
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

    (void)mkdir("/var/lib/locachy", 0755);
    (void)mkdir(bat_history_dir(), 0755);

    int ufd = open_uevent_socket();
    int sys_bats = record_snapshot_ex(1, 1);

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
        if (need_snapshot) {
            sys_bats = record_snapshot_ex(0, full_scan);
        }
        if (reload_flag) {
            reload_flag = 0;
            if (!need_snapshot || !full_scan) {
                sys_bats = record_snapshot_ex(0, 1);
            }
            reapply_rc_thresholds();
            flush_all_pending(1, current_epoch_ts());
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
    if (sfd >= 0) {
        close(sfd);
        sigprocmask(SIG_SETMASK, &oldmask, NULL);
    }
    if (ufd >= 0) close(ufd);
    return 0;
}
