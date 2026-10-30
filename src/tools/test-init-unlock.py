#!/usr/bin/env python3
"""Actual init unlock/resume functions with disks, token results and UI doubled.
No block devices, TPM, mounts or real hibernation are used. The LUKS metadata
is deliberately untrusted; only a successful unlock can authorize resume.
  python3 -B tools/test-init-unlock.py [SCRATCH_PARENT]
Requires a C compiler (cc).
"""
from pathlib import Path
import subprocess
import sys
import tempfile

SOURCE = Path(__file__).resolve().parents[1]
SCRATCH = sys.argv[1] if len(sys.argv) > 1 else None


def function(source, declaration):
    start = source.index(declaration)
    return source[start:source.index("\n}\n", start) + 3]


PREFIX = r'''
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#ifdef __linux__
#include <sys/sysmacros.h>
#endif
static char luks_root_dev[64], luks_swap_dev[64], resume_dev[16];
static const char *cryptsetup_path;
static int root_open, swap_open, resume_image, luks_only, luks_passphrase_entered;
static int root_found, swap_found, root_token_ok, swap_token_ok, pass_ok, image;
static int root_tokens, swap_tokens, root_passphrases, swap_passphrases, prompts, resume_writes;
static const char *metadata;
static char order[32];
static int check_luks_dev(const char *dev) { (void)dev; return 0; }
static void each_block_dev(int (*fn)(const char *)) {
    (void)fn;
    if (root_found) strcpy(luks_root_dev, "/fixture/root");
    if (swap_found) strcpy(luks_swap_dev, "/fixture/swap");
}
static int fixture_access(const char *path, int mode) { (void)path; (void)mode; return 0; }
static int fixture_mkdir(const char *path, mode_t mode) { (void)path; (void)mode; return 0; }
static void load_early_modules(const char *const *mods) { (void)mods; }
static void ensure_char_node_from_sysfs(const char *dev, const char *sys) { (void)dev; (void)sys; }
static void measure_phase(const char *phase) { (void)phase; }
static void say(const char *fmt, ...) { (void)fmt; }
static int run_cryptsetup(const char *cs, const char *dev, const char *mapper, const char *pass) {
    (void)cs; (void)mapper;
    int swap = strstr(dev, "swap") != NULL;
    if (pass) {
        if (swap) swap_passphrases++; else root_passphrases++;
        return pass_ok ? 0 : -1;
    }
    assert(strlen(order) + 2 < sizeof order);
    strcat(order, swap ? "S" : "R");
    if (swap) { swap_tokens++; return swap_token_ok ? 0 : -1; }
    root_tokens++; return root_token_ok ? 0 : -1;
}
static int has_signature(const char *dev, off_t offset, const char *sig) {
    (void)dev; (void)offset;
    return !strcmp(sig, image ? "S1SUSPEND" : "SWAPSPACE2");
}
static int swap_holds_image(void) {
    return luks_swap_dev[0] && swap_open && image;
}
static int has_label(const char *dev, off_t offset, const char *label) {
    (void)dev; (void)offset; (void)label; return 1;
}
static int is_mapper(const char *dev, const char *name) { (void)dev; (void)name; return 1; }
static int fixture_stat(const char *dev, struct stat *st) {
    (void)dev; st->st_rdev = makedev(253, 0); return 0;
}
static int write_file(const char *path, const char *value) {
    (void)value; assert(!strcmp(path, "/sys/power/resume")); resume_writes++; return 0;
}
static int screen_open(const char *font) { (void)font; return 1; }
static int keys_open(void) { return 1; }
static void keys_release(void) {}
static void screen_release(void) {}
static void ask_secret(const char *title, const char *label, const char *why, char *out, size_t size) {
    (void)title; (void)label; (void)why; prompts++; snprintf(out, size, "fixture passphrase");
}
/* has_tpm_token really forks and reads this metadata through its pipe. It
 * cannot authorize the cryptsetup fixture, whose root token result is independent. */
static int fixture_execl(const char *path, const char *arg, ...) {
    (void)path; (void)arg;
    size_t size = strlen(metadata);
    ssize_t n = write(STDOUT_FILENO, metadata, size);
    _exit(n == (ssize_t)size ? 0 : 1);
}
#define access fixture_access
#define mkdir fixture_mkdir
#define execl fixture_execl
#define stat(dev, st) fixture_stat(dev, st)
'''

SUFFIX = r'''
static void reset(const char *json) {
    root_found = swap_found = image = swap_token_ok = 1;
    root_token_ok = pass_ok = 0;
    root_tokens = swap_tokens = root_passphrases = swap_passphrases = prompts = resume_writes = 0;
    root_open = swap_open = resume_image = luks_only = luks_passphrase_entered = 0;
    luks_root_dev[0] = luks_swap_dev[0] = order[0] = '\0';
    cryptsetup_path = NULL; metadata = json;
}
static void try_resume(void) {
    luks_only = luks_root_dev[0] != '\0';
    check_resume_dev("/fixture/swap-mapper");
}
int main(void) {
    const char *forged[] = {
        "{\"tokens\": {}}",
        "{\"tokens\": {\"0\": {\"type\": \"locachy-tpm12\", \"keyslots\": [\"0\"], \"blob\": \"00\"}}}",
        "{\"tokens\": {\"0\": {\"type\": \"systemd-tpm2\", \"keyslots\": [\"0\"], \"tpm2-blob\": \"AA==\"}}}",
    };
    for (size_t i = 0; i < sizeof forged / sizeof forged[0]; i++) {
        reset(forged[i]);
        unlock_luks(); try_resume();
        assert(!root_open && root_tokens == 1 && swap_tokens == 1 && !strcmp(order, "SR"));
        assert(prompts == 5 && root_passphrases == 5 && !swap_passphrases);
        assert(!resume_image && !resume_writes);
    }
    puts("PASS missing/forged root TPM metadata never authorizes resume; five failed passphrases remain closed");

    reset(""); /* luksDump can find nothing, but the real token still works */
    root_token_ok = 1;
    unlock_luks(); try_resume();
    assert(root_open && swap_open && root_tokens == 1 && swap_tokens == 1 && !strcmp(order, "SR"));
    assert(resume_image && resume_writes == 1 && !prompts && !luks_passphrase_entered);
    puts("PASS successful root and swap token unlocks authorize resume without typing or metadata detection");

    reset(forged[1]); pass_ok = 1;
    unlock_luks(); try_resume();
    assert(root_open && prompts == 1 && root_passphrases == 1 && luks_passphrase_entered);
    assert(!resume_image && resume_writes == 1);
    puts("PASS rejected root token falls back to the correct long passphrase before resume");

    reset(forged[2]); swap_token_ok = 0; pass_ok = 1;
    unlock_luks(); try_resume();
    assert(root_open && swap_open && prompts == 1 && root_passphrases == 1 && swap_passphrases == 1);
    assert(resume_writes == 1 && luks_passphrase_entered);
    puts("PASS both rejected tokens require the long passphrase before encrypted resume");

    reset(forged[1]); root_found = 0;
    unlock_luks(); try_resume();
    assert(swap_open && !luks_root_dev[0] && !root_tokens && !prompts);
    assert(!resume_image && !resume_writes);
    puts("PASS encrypted swap with an unidentified root cannot authorize resume");

    reset(""); root_found = swap_found = 0;
    unlock_luks(); try_resume();
    assert(!root_tokens && !swap_tokens && !prompts && resume_writes == 1);
    puts("PASS unencrypted-system resume is unchanged");

    reset(""); root_token_ok = 1; image = 0;
    unlock_luks(); try_resume();
    assert(root_open && swap_open && !prompts && !resume_image && resume_writes == 1);
    puts("PASS cold TPM boot without a hibernation image is unchanged");
    return 0;
}
'''

source = (SOURCE / "init/init.c").read_text()
functions = ["static int has_tpm_token(", "static void unlock_with_passphrase(",
             "static const char *unlock_with_tpm(", "static void unlock_luks(", "static int check_resume_dev("]
with tempfile.TemporaryDirectory(prefix="init-unlock-", dir=SCRATCH) as tmp:
    root = Path(tmp)
    c = root / "test.c"
    c.write_text(PREFIX + "\n".join(function(source, name) for name in functions) + SUFFIX)
    subprocess.run(["cc", "-O2", "-Wall", "-Wextra", "-Werror", str(c), "-o", str(root / "test")],
                   check=True, timeout=60)
    subprocess.run([str(root / "test")], check=True, timeout=30)
