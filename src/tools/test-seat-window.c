/* locachy-seat --window: the top bar's requests for host programs as managed
 * windows, checked and passed on to process 1, without a compositor or a
 * process 1: a stand-in window-control socket and a socketpair for its link. */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#define main seat_main
#include "../progs/seat.c"
#undef main
#include <assert.h>

void say(const char *fmt, ...) { (void)fmt; }
double uptime(void) { return 0; }

/* What reached process 1, fields joined by '|'. */
static char forwarded[2048];
int control_send_fields(int sock, const char *const *fields)
{
    (void)sock;
    forwarded[0] = '\0';
    for (int i = 0; fields[i]; i++) {
        size_t n = strlen(forwarded);
        snprintf(forwarded + n, sizeof forwarded - n, "%s%s", i ? "|" : "", fields[i]);
    }
    return 0;
}

static void forward(int pair[2], const char *msg, size_t len, const char *expect)
{
    forwarded[0] = '\0';
    assert(send(pair[1], msg, len, 0) == (ssize_t)len);
    window_forward(pair[0], 7);
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

int main(void)
{
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
    assert(!window_request_valid("admin", "") && !window_request_valid("edit", "") &&
           !window_request_valid("sh", "") && !window_request_valid("", ""));
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
    forward(pair, "app\0open\0edge", 13, "app|open|edge");
    forward(pair, "app\0open-own-tor\0tor-browser", 28, "app|open-own-tor|tor-browser");
    forward(pair, "app\0stop\0edge", 13, "");
    forward(pair, "app\0open\0window-view-1", 22, "");
    forward(pair, "window\0view\0/home/u/a b.pdf", 27, "window|view|/home/u/a b.pdf");
    forward(pair, "window\0edit", 11, "");
    forward(pair, "window\0settings\0bogus", 21, "");
    forward(pair, "open\0view", 9, "");
    forward(pair, "window\0settings\0network\0x", 25, "");
    puts("PASS the seat passes on only valid window requests to process 1");

    char dir[] = "/tmp/seat-window-XXXXXX", got[2048];
    assert(mkdtemp(dir));
    setenv("XDG_RUNTIME_DIR", dir, 1);
    char *settings[] = { "settings", "network" }, *mixer[] = { "mixer" }, *view[] = { "view", "/home/u/a b.pdf" };
    assert(client(dir, 2, settings, "ok window requested", got, sizeof got) == 0);
    assert(strcmp(got, "window settings network") == 0);
    assert(client(dir, 1, mixer, "ok window requested", got, sizeof got) == 0);
    assert(strcmp(got, "window mixer") == 0);
    assert(client(dir, 2, view, "error the seat does not take window requests now", got, sizeof got) == 1);
    assert(strcmp(got, "window view /home/u/a b.pdf") == 0);
    char *edit[] = { "edit" }, *bad_page[] = { "settings", "bogus" };
    assert(window_client(1, edit) == 2 && window_client(2, bad_page) == 2 && window_client(0, edit) == 2);
    char sock[300];
    snprintf(sock, sizeof sock, "%s/locachy-control.sock", dir);
    unlink(sock);
    assert(window_client(2, settings) == 1); /* no window control: refused, not hung */
    rmdir(dir);
    puts("PASS locachy-seat --window sends one bounded request and reports the answer");
    return 0;
}
