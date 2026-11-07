/* Settings' window close (K_CLOSE), run by test-interaction-regressions.sh.
 * It leaves as q does, also under a package, repositories or boot image
 * job: those are system jobs (locachy-jobs), asked for here and run by the
 * service, which go on without settings; a new run of settings finds the
 * ones that have not ended and follows them again. A question that reads
 * the close is cancelled as Esc cancels it. The queue is a double
 * (jobs-double.h); the screen and the keys are the test's own.
 * Build with -ffunction-sections -fdata-sections -Wl,--gc-sections. */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <assert.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#define main settings_main
#include "../progs/settings.c"
#include "../base/fontmatch.c" /* without FreeType: fontconfig is not asked */
#include "i18n-stub.h"
#include "jobs-double.h"
#undef main

static int kept;
static const int *keys_in;

void key_close_kept(void) { kept++; }
int key_get(void) { return *keys_in++; }
void say(const char *fmt, ...) { (void)fmt; }
int rc_set(const char *name, const char *value) { (void)name; (void)value; return 0; }
int rc_read(void (*fn)(const char *, const char *, void *), void *arg) { (void)fn; (void)arg; return 0; }
int monitor_internal(const char *name) { (void)name; return 0; }
int monitor_profiles_connected(struct monitor_identity *out, int max) { (void)out; (void)max; abort(); }
int monitor_profiles_save(const struct monitor_profile_entry *entries, int n) { (void)entries; (void)n; abort(); }
double uptime(void) { static double t; return t += 10; }
int screen_cols(void) { return 100; }
int screen_rows(void) { return 30; }
int ui_cells(const char *s) { return (int)strlen(s); }
void screen_fill(int x, int y, int w, int h, uint32_t color) { (void)x; (void)y; (void)w; (void)h; (void)color; }
void screen_text(int x, int y, const char *text, uint32_t fg, uint32_t bg)
{ (void)x; (void)y; (void)text; (void)fg; (void)bg; }
void screen_flush(void) {}
void screen_bar(const char *title, const char *ime) { (void)title; (void)ime; abort(); }
int key_poll_timeout(int fd, int timeout) { (void)fd; (void)timeout; abort(); }
int screen_rescale(void) { abort(); }
int open_candidates(const char *type, char names[][OPEN_NAME], int max) { (void)type; (void)names; (void)max; abort(); }
int open_set_rule(const char *type, const char *value) { (void)type; (void)value; abort(); }
int open_containers(char names[][OPEN_NAME], int max) { (void)names; (void)max; abort(); }
int base_call(char *const argv[]) { (void)argv; abort(); }

int main(void)
{
    /* No job: the close is q's. */
    assert(close_read('q') == 0 && !closing);
    assert(close_read(K_CLOSE) == 1 && closing && close_due() && kept == 0);
    puts("PASS with no job running the close leaves at once, as q does");

    /* A package transaction: asked of the queue, which runs it; the close
     * leaves at once and asks nothing of the job. */
    closing = 0;
    jobs_double_ended = 0;
    start_pkg_job("install", "tailscale");
    assert(pkg_job == 7001 && !strcmp(jobs_double_last, "pkg-install tailscale"));
    assert(close_read(K_CLOSE) && closing && close_due() && kept == 0);
    pkg_check();
    assert(pkg_job == 7001); /* still running: it is the queue's, not the window's */
    puts("PASS a package job does not hold the close: settings leaves, the job is the queue's");

    /* The kinds settings asks for, with their arguments. */
    start_pkg_job("kernel", "select linux-zen");
    assert(!strcmp(jobs_double_last, "kernel-select linux-zen"));
    start_pkg_job("kernel", "headers on");
    assert(!strcmp(jobs_double_last, "kernel-headers on"));
    start_pkg_job("swap", "python-dlib python-dlib-cuda");
    assert(!strcmp(jobs_double_last, "pkg-swap python-dlib python-dlib-cuda"));
    start_pkg_job("remove", "tor lyrebird-proxy");
    assert(!strcmp(jobs_double_last, "pkg-remove tor lyrebird-proxy"));
    start_repos_job("extra=off");
    assert(repos_job && !strcmp(jobs_double_last, "repos-set extra=off"));
    start_repos_job("rank");
    assert(repos_job && !strcmp(jobs_double_last, "repos-rank"));
    make_boot_image();
    assert(boot_job && !strcmp(jobs_double_last, "boot") && strstr(notice, "Making the boot image"));
    puts("PASS package, kernel, repositories and boot image jobs go to the queue as their kinds");

    /* A new run of settings: the queue's jobs it asked for that have not
     * ended are followed again; once they end, it says how. */
    pkg_job = repos_job = boot_job = 0;
    pkg_action[0] = pkg_name[0] = repos_what[0] = '\0';
    jobs_double_mine[0] = (struct jobs_row){ .id = 7001, .kind = "pkg-install", .state = "running", .args = "tailscale" };
    jobs_double_mine[1] = (struct jobs_row){ .id = 7006, .kind = "repos-set", .state = "queued", .args = "extra=off" };
    jobs_double_mine[2] = (struct jobs_row){ .id = 7008, .kind = "boot", .state = "done" };
    jobs_double_nmine = 3;
    adopt_jobs();
    assert(pkg_job == 7001 && !strcmp(pkg_action, "install") && !strcmp(pkg_name, "tailscale"));
    assert(repos_job == 7006 && !strcmp(repos_what, "extra=off") && !boot_job); /* the boot image's has ended */
    jobs_double_ended = 1;
    pkg_check();
    assert(!pkg_job && strstr(notice, "installed"));
    puts("PASS reopened, settings follows the jobs it asked for again, and says how they ended");

    /* A question that reads the close: cancelled as Esc, under a job too. */
    closing = kept = 0;
    jobs_double_ended = 0;
    start_pkg_job("remove", "tailscale");
    char text[64] = "abc";
    keys_in = (const int[]){ 'x', K_CLOSE };
    assert(prompt_as("Name: ", text, sizeof text, 0) == 0 && closing && kept == 0 && close_due());
    closing = 0;
    keys_in = (const int[]){ K_CLOSE };
    assert(prompt("Name: ", text, sizeof text) == 0 && closing && kept == 0 && close_due());
    puts("PASS a question cancelled by the close: due at once, under a job too");
    puts("settings: the window's close passed.");
    return 0;
}
