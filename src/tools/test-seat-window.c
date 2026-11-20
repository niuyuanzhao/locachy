/* Seat window routing and the private bar link, without a compositor or
 * process 1: a stand-in window-control socket and socketpairs. */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#define main seat_main
#include "../progs/seat.c"
#undef main
#include <assert.h>

void say(const char *fmt, ...) { (void)fmt; }
double uptime(void) { return 0; }

/* What reached the fake process 1, fields joined by '|'. Bar messages
 * travel on the real private socketpair with the control packet framing. */
static char forwarded[2048];
static const int fake_ctl = INT_MAX;
int control_send(int sock, const char *msg, size_t len, const int *fds, int nfds)
{
    (void)fds;
    assert(nfds == 0);
    return send(sock, msg, len, MSG_NOSIGNAL) < 0 ? -1 : 0;
}
int control_send_fields(int sock, const char *const *fields)
{
    if (sock == bar_keys) {
        char msg[64];
        size_t len = 0;
        for (; *fields; fields++) {
            size_t n = strlen(*fields) + 1;
            assert(len + n <= sizeof msg);
            memcpy(msg + len, *fields, n);
            len += n;
        }
        return control_send(sock, msg, len - 1, NULL, 0);
    }
    assert(sock == fake_ctl);
    forwarded[0] = '\0';
    for (int i = 0; fields[i]; i++) {
        size_t n = strlen(forwarded);
        snprintf(forwarded + n, sizeof forwarded - n, "%s%s", i ? "|" : "", fields[i]);
    }
    return 0;
}

/* The bar's link, as base/control.c reads it (no descriptors here). */
int control_recv(int sock, char *msg, size_t size, int *fds, int *nfds)
{
    (void)fds;
    *nfds = 0;
    ssize_t n = recv(sock, msg, size - 1, MSG_DONTWAIT);
    if (n >= 0) msg[n] = '\0';
    return (int)n;
}
int control_fields(char *msg, int len, char **fields, int max)
{
    int n = 0;
    for (int i = 0; i < len && n < max; i += (int)strlen(msg + i) + 1) fields[n++] = msg + i;
    return n;
}

static void forward(int pair[2], const char *msg, size_t len, const char *expect)
{
    forwarded[0] = '\0';
    assert(send(pair[1], msg, len, 0) == (ssize_t)len);
    window_forward(pair[0], fake_ctl);
    assert(strcmp(forwarded, expect) == 0);
}

/* The client against a stand-in labwc-control socket: what it sent, and its
 * status for the answer given. */
static int client(const char *dir, int argc, char **argv, const char *answer, char *got, size_t size)
{
    struct sockaddr_un address = { .sun_family = AF_UNIX };
    assert(snprintf(address.sun_path, sizeof address.sun_path, "%s/locachy-control.sock", dir) <
           (int)sizeof address.sun_path);
    unlink(address.sun_path);
    int listener = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0);
    assert(listener >= 0 && bind(listener, (struct sockaddr *)&address, sizeof address) == 0 &&
           listen(listener, 1) == 0);
    pid_t pid = fork();
    if (pid == 0) {
        close(listener);
        _exit(window_client(argc, argv));
    }
    got[0] = '\0';
    struct pollfd p = { .fd = listener, .events = POLLIN };
    if (poll(&p, 1, 2000) > 0) {
        int c = accept(listener, NULL, NULL);
        ssize_t n = recv(c, got, size - 1, 0);
        got[n > 0 ? n : 0] = '\0';
        assert(send(c, answer, strlen(answer), 0) == (ssize_t)strlen(answer));
        close(c);
    }
    close(listener);
    int status = 0;
    assert(waitpid(pid, &status, 0) == pid && WIFEXITED(status));
    return WEXITSTATUS(status);
}

static void wait_command(const char *packet, size_t len, int taking)
{
    char msg[64];
    assert(len < sizeof msg);
    memcpy(msg, packet, len);
    msg[len] = '\0';
    forwarded[0] = '\0';
    seat_control_message(fake_ctl, msg, (int)len, NULL, 0, taking);
}

static void expect_packet(int fd, const char *packet, size_t len)
{
    char msg[64];
    ssize_t got = recv(fd, msg, sizeof msg, MSG_DONTWAIT);
    if (!len) assert(got < 0 && errno == EAGAIN);
    else assert(got == (ssize_t)len && memcmp(msg, packet, len) == 0);
}

static void desktop_report(int fd, const char *packet, size_t len, const char *expect)
{
    assert(send(fd, packet, len, 0) == (ssize_t)len);
    forwarded[0] = '\0';
    bar_reports(fake_ctl);
    assert(strcmp(forwarded, expect) == 0);
}

static void desktop_bridge(void)
{
    static const char visible[] = "desktop-wait\0" "18446744073709551615", hide[] = "desktop-wait\0" "0";
    static const char overflow[] = "desktop-wait\0" "18446744073709551616", fresh[] = "desktop-wait\0" "42";
    static const char query[] = "desktop-wait?", close_max[] = "desktop-close\0" "18446744073709551615";
    static const char stale[] = "desktop-close\0" "42", extra[] = "desktop-close\0" "18446744073709551615\0extra";
    int bp[2];
    assert(socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC | SOCK_NONBLOCK, 0, bp) == 0);
    bar_keys = bp[0];
    active_seat = 1;
    wait_command(visible, sizeof visible - 1, 0);
    expect_packet(bp[1], visible, sizeof visible - 1);
    wait_command(overflow, sizeof overflow - 1, 0);
    expect_packet(bp[1], NULL, 0);
    desktop_report(bp[1], query, sizeof query - 1, "");
    expect_packet(bp[1], visible, sizeof visible - 1);
    desktop_report(bp[1], stale, sizeof stale - 1, "");
    desktop_report(bp[1], extra, sizeof extra - 1, "");
    desktop_report(bp[1], close_max, sizeof close_max - 1, "desktop-close|18446744073709551615");

    seat_release(fake_ctl);
    expect_packet(bp[1], hide, sizeof hide - 1);
    desktop_report(bp[1], query, sizeof query - 1, "");
    desktop_report(bp[1], close_max, sizeof close_max - 1, "");
    expect_packet(bp[1], NULL, 0);
    seat_activate(fake_ctl);
    expect_packet(bp[1], NULL, 0);
    desktop_report(bp[1], close_max, sizeof close_max - 1, "");
    seat_release(fake_ctl);
    wait_command(fresh, sizeof fresh - 1, 1); /* fresh wait queued before DRM master's reply */
    expect_packet(bp[1], NULL, 0);
    desktop_report(bp[1], query, sizeof query - 1, "");
    desktop_report(bp[1], stale, sizeof stale - 1, "");
    expect_packet(bp[1], NULL, 0);
    seat_activate(fake_ctl);
    expect_packet(bp[1], fresh, sizeof fresh - 1);
    wait_command(hide, sizeof hide - 1, 0);
    expect_packet(bp[1], hide, sizeof hide - 1);
    desktop_report(bp[1], stale, sizeof stale - 1, "");
    puts("PASS private desktop wait validates uint64, replays after exec, and gates close across handover");

    close(bp[1]); /* before capture: report loss now, and on every later nonzero wait */
    bar_reports(fake_ctl);
    assert(bar_keys == -1 && strcmp(forwarded, "desktop-wait-lost") == 0);
    wait_command(fresh, sizeof fresh - 1, 0);
    assert(desktop_wait_generation == 0 && strcmp(forwarded, "desktop-wait-lost") == 0);
    wait_command(hide, sizeof hide - 1, 0);
    assert(forwarded[0] == '\0');
    assert(socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC | SOCK_NONBLOCK, 0, bp) == 0);
    bar_keys = bp[0];
    close(bp[1]); /* peer closed before polling: publishing must still detect failure */
    wait_command(fresh, sizeof fresh - 1, 0);
    assert(bar_keys == -1 && desktop_wait_generation == 0 && strcmp(forwarded, "desktop-wait-lost") == 0);
    puts("PASS private desktop wait reports closed or unavailable bar without restarting it");
}

int main(void)
{
    desktop_bridge();
    assert(window_request_valid("settings", ""));
    static const char *const pages[] = { "network", "power", "devices", "date-time", "hardware" };
    for (size_t i = 0; i < sizeof pages / sizeof *pages; i++) assert(window_request_valid("settings", pages[i]));
    assert(!window_request_valid("settings", "bogus"));
    assert(!window_request_valid("settings", "network\n"));
    assert(window_request_valid("view", "") && window_request_valid("view", "/home/u/a b.pdf"));
    assert(!window_request_valid("view", "a.pdf"));
    assert(!window_request_valid("view", "/a\tb"));
    assert(window_request_valid("files", "") && window_request_valid("files", "/home/u/My Files"));
    assert(!window_request_valid("files", "home/u"));
    assert(window_request_valid("mixer", "") && !window_request_valid("mixer", "/usr/bin/sh"));
    assert(window_request_valid("bluetooth", "") && !window_request_valid("bluetooth", "/usr/bin/sh"));
    assert(window_request_valid("network", "") && !window_request_valid("network", "wlan0"));
    assert(window_request_valid("term", "") && !window_request_valid("term", "/bin/sh"));
    assert(window_request_valid("edit", "") && window_request_valid("edit", "/home/u/new.txt"));
    assert(window_request_valid("archive", "") && window_request_valid("archive", "/home/u/bundle.tar.gz"));
    assert(window_request_valid("packages", "") && !window_request_valid("packages", "/x"));
    assert(window_request_valid("firmware", "") && !window_request_valid("firmware", "/x"));
    assert(!window_request_valid("admin", "") && !window_request_valid("sh", "") && !window_request_valid("", ""));
    assert(app_request_valid("edge") && app_request_valid("tor-browser") && !app_request_valid("window-view-1") &&
           !app_request_valid("../edge") && !app_request_valid(""));
    char long_target[WINDOW_TARGET_MAX + 2];
    long_target[0] = '/';
    memset(long_target + 1, 'a', WINDOW_TARGET_MAX);
    long_target[WINDOW_TARGET_MAX + 1] = '\0';
    assert(!window_request_valid("view", long_target));
    puts("PASS window requests: four fixed programs, settings pages, absolute paths for view and files");

    int pair[2];
    assert(socketpair(AF_UNIX, SOCK_SEQPACKET, 0, pair) == 0);
    forward(pair, "window\0settings\0network", 23, "window|settings|network");
    forward(pair, "window\0mixer", 12, "window|mixer");
    forward(pair, "window\0bluetooth", 16, "window|bluetooth");
    forward(pair, "window\0network", 14, "window|network");
    forward(pair, "window\0term", 11, "window|term");
    forward(pair, "window\0edit\0/home/u/a.txt", sizeof("window\0edit\0/home/u/a.txt"), "window|edit|/home/u/a.txt");
    forward(pair, "window\0archive\0/home/u/a.tar", sizeof("window\0archive\0/home/u/a.tar"), "window|archive|/home/u/a.tar");
    forward(pair, "window\0packages", sizeof("window\0packages"), "window|packages");
    forward(pair, "window\0firmware", sizeof("window\0firmware"), "window|firmware");
    forward(pair, "app\0open\0edge", 13, "app|open|edge");
    forward(pair, "app\0open-own-tor\0tor-browser", 28, "app|open-own-tor|tor-browser");
    forward(pair, "app\0stop\0edge", 13, "");
    forward(pair, "app\0open\0window-view-1", 22, "");
    forward(pair, "window\0view\0/home/u/a b.pdf", 27, "window|view|/home/u/a b.pdf");
    forward(pair, "window\0admin", 12, "");
    forward(pair, "window\0settings\0bogus", 21, "");
    forward(pair, "open\0view", 9, "");
    forward(pair, "window\0settings\0network\0x", 25, "");
    puts("PASS the seat passes on only valid window requests to process 1");

    active_seat = 1;
    idle_blanked = 0;
    forward(pair, "idle-lock", sizeof("idle-lock") - 1, "idle-lock");
    forward(pair, "idle-blanked", sizeof("idle-blanked") - 1, "idle-blanked");
    assert(idle_blanked);
    active_seat = 0;
    forward(pair, "idle-lock", sizeof("idle-lock") - 1, "");
    active_seat = 1;
    forward(pair, "idle-power-lost", sizeof("idle-power-lost") - 1, "idle-power-lost");
    assert(!idle_blanked);
    forward(pair, "idle-unblanked", sizeof("idle-unblanked") - 1, "");
    assert(!idle_blanked);
    {
        int bp[2], rp[2];
        char got[32] = "";
        assert(socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC | SOCK_NONBLOCK, 0, bp) == 0);
        assert(socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC | SOCK_NONBLOCK, 0, rp) == 0);
        bar_keys = bp[0]; window_requests = rp[0]; is_labwc = 1; active_seat = 1;
        assert(send(bp[1], "activity", 8, 0) == 8);
        bar_reports(-1);
        assert(recv(rp[1], got, sizeof got - 1, MSG_DONTWAIT) == 12 && strcmp(got, "bar-activity") == 0);
        active_seat = 0;
        assert(send(bp[1], "activity", 8, 0) == 8);
        bar_reports(-1);
        assert(recv(rp[1], got, sizeof got - 1, MSG_DONTWAIT) < 0);
        bar_keys = window_requests = -1; is_labwc = 0;
        close(bp[0]); close(bp[1]); close(rp[0]); close(rp[1]);
    }
    puts("PASS private seat lifecycle forwards distinct idle lock and display-power states");

    char dir[] = "/tmp/seat-window-XXXXXX", got[2048];
    assert(mkdtemp(dir));
    setenv("XDG_RUNTIME_DIR", dir, 1);
    char *settings[] = { "settings", "network" }, *mixer[] = { "mixer" }, *view[] = { "view", "/home/u/a b.pdf" }, *edit[] = { "edit" };
    assert(client(dir, 2, settings, "ok window requested", got, sizeof got) == 0);
    assert(strcmp(got, "window settings network") == 0);
    assert(client(dir, 1, mixer, "ok window requested", got, sizeof got) == 0);
    assert(strcmp(got, "window mixer") == 0);
    assert(client(dir, 1, edit, "ok window requested", got, sizeof got) == 0);
    assert(strcmp(got, "window edit") == 0);
    assert(client(dir, 2, view, "error the seat does not take window requests now", got, sizeof got) == 1);
    assert(strcmp(got, "window view /home/u/a b.pdf") == 0);
    char *admin[] = { "admin" }, *bad_page[] = { "settings", "bogus" };
    assert(window_client(1, admin) == 2 && window_client(2, bad_page) == 2 && window_client(0, admin) == 2);
    char sock[300];
    snprintf(sock, sizeof sock, "%s/locachy-control.sock", dir);
    unlink(sock);
    assert(window_client(2, settings) == 1); /* no window control: refused, not hung */
    rmdir(dir);
    puts("PASS locachy-seat --window sends one bounded request and reports the answer");
    return 0;
}
