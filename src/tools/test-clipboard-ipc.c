/* Local clipboard API: a private daemon, no display, no user content. */
#define _GNU_SOURCE
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>
#include "clipboard.h"

static void text_is(const char *text, size_t size, int primary)
{
    int fd;
    size_t got;
    assert(clipboard_get(primary, &fd, &got) == CLIPBOARD_OK && got == size && fd >= 0);
    char *buf = malloc(size + 1);
    assert(buf && pread(fd, buf, size, 0) == (ssize_t)size && memcmp(buf, text, size) == 0);
    close(fd); free(buf);
}

int main(int argc, char **argv)
{
    assert(argc == 3);
    char socket_path[512];
    snprintf(socket_path, sizeof socket_path, "%s/ipc.sock", argv[2]);
    assert(setenv("LOCACHY_CLIPBOARD_SOCKET", socket_path, 1) == 0);
    assert(setenv("XDG_RUNTIME_DIR", argv[2], 1) == 0);
    assert(setenv("WAYLAND_DISPLAY", "does-not-exist", 1) == 0);
    pid_t pid = fork();
    if (!pid) { execl(argv[1], argv[1], "--limit", "1048576", (char *)NULL); _exit(127); }
    assert(pid > 0);
    struct stat st;
    for (int i = 0; i < 100 && stat(socket_path, &st) < 0; i++) usleep(10000);
    int ready = 0;
    for (int i = 0; i < 100; i++) {
        if (stat(socket_path, &st) == 0 && (st.st_mode & 0777) == 0600) { ready = 1; break; }
        usleep(10000);
    }
    assert(ready);
    int fd;
    size_t size;
    assert(clipboard_get(0, &fd, &size) == CLIPBOARD_EMPTY);
    const char text[] = "中文 🌍\n";
    assert(clipboard_set(0, text, sizeof text - 1) == CLIPBOARD_OK);
    text_is(text, sizeof text - 1, 0);
    assert(clipboard_set(1, "primary", 7) == CLIPBOARD_OK);
    text_is(text, sizeof text - 1, 0);
    text_is("primary", 7, 1);
    assert(clipboard_set(0, "", 0) == CLIPBOARD_OK);
    text_is("", 0, 0);
    uint64_t token;
    assert(clipboard_sync(&token) == CLIPBOARD_OK && clipboard_check(token) == CLIPBOARD_OK);
    assert(clipboard_set(0, text, sizeof text - 1) == CLIPBOARD_OK);
    assert(clipboard_check(token) == CLIPBOARD_BUSY);
    assert(clipboard_clear(0) == CLIPBOARD_OK);
    assert(clipboard_get(0, &fd, &size) == CLIPBOARD_EMPTY);
    puts("PASS private root-only socket, UTF-8, empty text, clear, independent primary and generation check");
    char *large = calloc(1048577, 1);
    assert(large && clipboard_set(0, large, 1048577) == CLIPBOARD_FAILED);
    free(large);
    assert(clipboard_set(0, "recovered", 9) == CLIPBOARD_OK);
    text_is("recovered", 9, 0);
    puts("PASS oversized input is rejected without poisoning the local service");
    assert(kill(pid, SIGTERM) == 0);
    int status;
    assert(waitpid(pid, &status, 0) == pid && WIFEXITED(status) && WEXITSTATUS(status) == 0);
    assert(access(socket_path, F_OK) < 0);
    puts("ALL PASSED");
    return 0;
}
