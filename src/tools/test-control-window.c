/* labwc-control's part of window requests (window PROGRAM [TARGET]): passed
 * to the seat as process 1 takes them, bounded; without a compositor. */
#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <wayland-client.h>
#define main controller_main
#include "../progs/labwc-control.c"
#undef main
void say(const char *fmt, ...) { (void)fmt; }
double uptime(void) { return 100; }
int rc_read(void (*fn)(const char *, const char *, void *), void *arg) { (void)fn; (void)arg; return 0; }

static void request(int pair[2], const char *args, const char *reply_start, const char *msg, size_t len)
{
    char reply[160], got[2048];
    window_request(args, reply, sizeof reply);
    assert(strncmp(reply, reply_start, strlen(reply_start)) == 0);
    ssize_t n = recv(pair[1], got, sizeof got, MSG_DONTWAIT);
    if (msg) {
        assert(n == (ssize_t)len && memcmp(got, msg, len) == 0);
    } else {
        assert(n < 0);
    }
}

static void idle_power_policy(void)
{
    struct idle_rc rc = { .minutes = 10, .auto_lock = 1, .blank_after = 5 };
    assert(rc.auto_blank == 0 && rc.blank_after == 5); /* opt-in, safe default */
    idle_rc_setting("auto_blank", "1", &rc);
    assert(rc.auto_blank == 1);
    idle_rc_setting("blank_after", "1", &rc);
    assert(rc.blank_after == 1);
    idle_rc_setting("blank_after", "60", &rc);
    assert(rc.blank_after == 60);
    static const char *const invalid[] = { "0", "61", "-1", "5x", "" };
    for (size_t i = 0; i < sizeof invalid / sizeof *invalid; i++) {
        idle_rc_setting("blank_after", invalid[i], &rc);
        assert(rc.blank_after == 5);
    }
    idle_rc_setting("auto_blank", "0", &rc);
    assert(rc.auto_blank == 0);

    struct power_output output = { 0 };
    power_mode(&output, NULL, ZWLR_OUTPUT_POWER_V1_MODE_OFF);
    assert(output.known && !output.on && !output.owned_off); /* pre-existing off is not ours */
    output.requested_off = 1;
    power_mode(&output, NULL, ZWLR_OUTPUT_POWER_V1_MODE_OFF);
    assert(output.owned_off);
    power_mode(&output, NULL, ZWLR_OUTPUT_POWER_V1_MODE_ON);
    assert(!output.owned_off); /* compositor already restored it */
    {
        int sp[2];
        assert(socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC | SOCK_NONBLOCK, 0, sp) == 0);
        seat_requests = sp[0];
        power_phase = POWER_BLANKING;
        power_barrier = (struct wl_callback *)1;
        power_cancelled = 0;
        assert(send(sp[1], "bar-activity", 12, 0) == 12);
        seat_request();
        assert(power_cancelled == 1);
        power_phase = POWER_AWAKE;
        power_barrier = NULL;
        power_cancelled = 0;
        seat_requests = -1;
        close(sp[0]); close(sp[1]);
    }
    puts("PASS shared display-off is opt-in, bounds blank_after, and owns only a confirmed OFF request");
}

int main(void)
{
    idle_power_policy();
    int pair[2];
    char reply[160];
    window_request("settings network", reply, sizeof reply);
    assert(strncmp(reply, "error no seat", 13) == 0); /* not started by a seat */
    assert(socketpair(AF_UNIX, SOCK_SEQPACKET, 0, pair) == 0);
    seat_requests = pair[0];
    request(pair, "settings network", "ok", "window\0settings\0network", 23);
    request(pair, "mixer", "ok", "window\0mixer", 12);
    request(pair, "bluetooth", "ok", "window\0bluetooth", 16);
    request(pair, "network", "ok", "window\0network", 14);
    request(pair, "term", "ok", "window\0term", 11);
    open_request("edge", 0, reply, sizeof reply);
    assert(strncmp(reply, "ok", 2) == 0);
    { char got[64]; assert(recv(pair[1], got, sizeof got, MSG_DONTWAIT) == 13 && memcmp(got, "app\0open\0edge", 13) == 0); }
    open_request("window-view-1", 0, reply, sizeof reply);
    assert(strncmp(reply, "error malformed", 15) == 0);
    request(pair, "view /home/u/a b.pdf", "ok", "window\0view\0/home/u/a b.pdf", 27);
    request(pair, "", "error malformed", NULL, 0);
    request(pair, " network", "error malformed", NULL, 0);
    char long_program[80];
    memset(long_program, 'p', 79);
    long_program[79] = '\0';
    request(pair, long_program, "error malformed", NULL, 0);
    puts("PASS labwc-control passes window requests to the seat as fields, bounded");
    close(pair[1]);
    request(pair, "mixer", "error the seat does not take", NULL, 0);
    puts("PASS a seat that is gone: the request is refused, not lost silently");
    return 0;
}
