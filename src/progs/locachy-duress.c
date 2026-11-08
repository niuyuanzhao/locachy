/*
 * locachy-duress (/usr/local/bin): the duress password's verifier, for
 * settings (tools/DURESS.md). Root only.
 *   locachy-duress status   "set" or "not set", and whether the disk can be
 *                           wiped ("luks" or "no-luks"); exit 0
 *   locachy-duress set      asks for it twice on the terminal; refuses one
 *                           shorter than DURESS_MIN_LENGTH, the login
 *                           password of uid 1000, or one that opens the
 *                           disk (cryptsetup --test-passphrase), and a
 *                           system without LOCachy's LUKS2 partitions
 *   locachy-duress clear    removes it
 * Arming it (rc.conf duress_enabled) and putting the verifier in the boot
 * image (locachy-boot) are settings' and locachy-boot's.
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <errno.h>
#include <fcntl.h>
#include <pwd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>

#include "duress.h"

static int read_hidden(const char *prompt, char *out, size_t size)
{
    int tty = open("/dev/tty", O_RDWR | O_NOCTTY | O_CLOEXEC);
    struct termios was, quiet;
    if (tty < 0 || tcgetattr(tty, &was) < 0) {
        if (tty >= 0) close(tty);
        return -1;
    }
    quiet = was;
    quiet.c_lflag &= (tcflag_t)~ECHO;
    tcsetattr(tty, TCSAFLUSH, &quiet);
    dprintf(tty, "%s", prompt);
    size_t len = 0;
    char c;
    while (len < size - 1 && read(tty, &c, 1) == 1 && c != '\n' && c != '\r') {
        out[len++] = c;
    }
    out[len] = '\0';
    tcsetattr(tty, TCSAFLUSH, &was);
    dprintf(tty, "\n");
    close(tty);
    return 0;
}

/* 1 when pw opens a LOCachy LUKS2 partition (the boot passphrase), tested
 * with cryptsetup on the same partitions the wipe destroys. */
static int opens_disk(const char *pw)
{
    char paths[4][160];
    int n = duress_luks_paths(paths, 4);
    for (int i = 0; i < n; i++) {
        int in[2];
        if (pipe2(in, O_CLOEXEC) < 0) return 1; /* cannot check: refuse */
        pid_t pid = fork();
        if (pid == 0) {
            dup2(in[0], STDIN_FILENO);
            int null = open("/dev/null", O_WRONLY | O_CLOEXEC);
            if (null >= 0) {
                dup2(null, STDOUT_FILENO);
                dup2(null, STDERR_FILENO);
            }
            execl("/usr/bin/cryptsetup", "cryptsetup", "open", "--test-passphrase", "--key-file=-", paths[i],
                  (char *)NULL);
            _exit(127);
        }
        close(in[0]);
        if (write(in[1], pw, strlen(pw)) < 0) {
            /* cryptsetup reads nothing, and refuses */
        }
        close(in[1]);
        int status = 1;
        if (pid > 0) waitpid(pid, &status, 0);
        if (WIFEXITED(status) && WEXITSTATUS(status) == 0) return 1;
    }
    return 0;
}

int main(int argc, char **argv)
{
    if (argc != 2) {
        fprintf(stderr, "usage: locachy-duress status | set | clear\n");
        return 2;
    }
    if (strcmp(argv[1], "status") == 0) {
        printf("%s %s\n", duress_configured() ? "set" : "not-set", duress_has_luks() ? "luks" : "no-luks");
        return 0;
    }
    if (geteuid() != 0) {
        fprintf(stderr, "locachy-duress: root only\n");
        return 1;
    }
    if (strcmp(argv[1], "clear") == 0) {
        if (duress_clear() < 0) {
            fprintf(stderr, "locachy-duress: cannot remove %s: %s\n", DURESS_FILE, strerror(errno));
            return 1;
        }
        printf("The duress password is removed.\n");
        return 0;
    }
    if (strcmp(argv[1], "set") != 0) {
        fprintf(stderr, "usage: locachy-duress status | set | clear\n");
        return 2;
    }
    if (!duress_has_luks()) {
        fprintf(stderr, "No encrypted disk (LUKS2 locachy-crypt): a duress password could not wipe anything. "
                        "Not set.\n");
        return 1;
    }
    char first[256], second[256];
    struct passwd *pw = getpwuid(1000);
    int result = 1;
    if (read_hidden("Duress password: ", first, sizeof first) < 0 ||
        read_hidden("Again: ", second, sizeof second) < 0) {
        fprintf(stderr, "locachy-duress: no terminal to ask on\n");
    } else if (strcmp(first, second) != 0) {
        fprintf(stderr, "They differ. Not set.\n");
    } else if (strlen(first) < DURESS_MIN_LENGTH) {
        fprintf(stderr, "Shorter than %d characters. Not set.\n", DURESS_MIN_LENGTH);
    } else if (pw && duress_login_check(pw->pw_name, first) != 0) {
        fprintf(stderr, "That is the login password (or it cannot be checked): it would unlock, never wipe. Not set.\n");
    } else if (opens_disk(first)) {
        fprintf(stderr, "That opens the disk: at power-on it would unlock, never wipe. Not set.\n");
    } else if (duress_set(first) < 0) {
        fprintf(stderr, "locachy-duress: cannot write %s: %s\n", DURESS_FILE, strerror(errno));
    } else {
        printf("The duress password is set.\n");
        result = 0;
    }
    explicit_bzero(first, sizeof first);
    explicit_bzero(second, sizeof second);
    return result;
}
