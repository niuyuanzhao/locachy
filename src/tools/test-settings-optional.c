/* Settings' optional components (Settings > System), run by
 * test-interaction-regressions.sh. A component that names a program or a
 * service in rc.conf is installed only when those lines are there as well as
 * its packages: FFmpeg present for something else is not the extended display
 * turned on, and Enter on the row then turns it on (the program line, the
 * package job, process 1 reloaded) instead of removing FFmpeg. rc.conf, the
 * probe file and process 1 are the test's own; the queue is a double.
 * Build with -ffunction-sections -fdata-sections -Wl,--gc-sections. */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <assert.h>
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

int settings_access(const char *path, int mode);
int settings_kill(pid_t pid, int sig);
#define access settings_access
#define kill settings_kill
#define main settings_main
#include "../progs/settings.c"
#undef main
#undef kill
#undef access
#include "../base/fontmatch.c" /* without FreeType: fontconfig is not asked */
#include "i18n-stub.h"
#include "jobs-double.h"

#define FFMPEG_PROBE "/usr/include/libavcodec/avcodec.h"
#define AUDIO_PROBE "/usr/bin/wiremix"

static int ffmpeg_present, audio_present, reloads;
static char rc_name[8][64], rc_value[8][256];
static int rc_count;

void key_close_kept(void) {}
int key_get(void) { abort(); }
void say(const char *fmt, ...) { (void)fmt; }

int rc_set(const char *name, const char *value)
{
    int i = 0;
    while (i < rc_count && strcmp(rc_name[i], name) != 0) i++;
    if (!value) {
        if (i < rc_count) {
            for (int k = i; k + 1 < rc_count; k++) {
                memcpy(rc_name[k], rc_name[k + 1], sizeof rc_name[k]);
                memcpy(rc_value[k], rc_value[k + 1], sizeof rc_value[k]);
            }
            rc_count--;
        }
        return 0;
    }
    if (i == rc_count) {
        assert(rc_count < 8);
        snprintf(rc_name[rc_count++], sizeof rc_name[0], "%s", name);
    }
    snprintf(rc_value[i], sizeof rc_value[i], "%s", value);
    return 0;
}

/* Called through a volatile pointer, as rc.c's is from another file: inlined,
 * settings' own callbacks would be checked against this table's sizes. */
static void (*volatile rc_callback)(const char *, const char *, void *);

int rc_read(void (*fn)(const char *, const char *, void *), void *arg)
{
    rc_callback = fn;
    for (int i = 0; i < rc_count; i++) rc_callback(rc_name[i], rc_value[i], arg);
    return 0;
}

static const char *test_rc(const char *name)
{
    for (int i = 0; i < rc_count; i++)
        if (strcmp(rc_name[i], name) == 0) return rc_value[i];
    return NULL;
}

int settings_access(const char *path, int mode)
{
    if (strcmp(path, FFMPEG_PROBE) == 0 || strcmp(path, AUDIO_PROBE) == 0) {
        if (strcmp(path, FFMPEG_PROBE) == 0 ? ffmpeg_present : audio_present) return 0;
        errno = ENOENT;
        return -1;
    }
    return access(path, mode);
}

int settings_kill(pid_t pid, int sig)
{
    assert(pid == 1 && sig == SIGHUP); /* process 1 reloads rc.conf; nothing else is signalled here */
    reloads++;
    return 0;
}

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
int key_poll_many(struct pollfd *extra, size_t count, int timeout) { (void)extra; (void)count; (void)timeout; abort(); }
int screen_rescale(void) { abort(); }
int open_candidates(const char *type, char names[][OPEN_NAME], int max) { (void)type; (void)names; (void)max; abort(); }
int open_set_rule(const char *type, const char *value) { (void)type; (void)value; abort(); }
int open_containers(char names[][OPEN_NAME], int max) { (void)names; (void)max; abort(); }
int base_call(char *const argv[]) { (void)argv; abort(); }

int main(void)
{
    int ext = -1, audio = -1;
    for (int i = 0; i < NOPT_MODULES; i++) {
        if (strcmp(opt_modules[i].label, "Extended display (extdisplay)") == 0) ext = i;
        if (strcmp(opt_modules[i].label, "Audio (PipeWire)") == 0) audio = i;
    }
    assert(ext >= 0 && audio >= 0);
    rc_set("service_jobs", "/usr/local/lib/locachy/locachy-jobs");

    /* FFmpeg there for something else, no program line: not installed. */
    ffmpeg_present = 1;
    assert(!opt_module_installed(ext));
    puts("PASS FFmpeg present without program_extdisplay is not the extended display installed");

    /* Enter turns it on: nothing is removed. */
    selected = opt_index() + ext;
    adjust(+1);
    assert(test_rc("program_extdisplay") && strcmp(test_rc("program_extdisplay"), "/usr/local/bin/extdisplay") == 0);
    assert(strcmp(jobs_double_last, "pkg-install ffmpeg json-c") == 0 && reloads == 1);
    assert(opt_module_installed(ext));
    puts("PASS Enter on it then writes the program line, asks for the package job and reloads process 1");

    /* Installed: Enter turns it off, the line first, then the package job. */
    pkg_job = 0;
    pkg_action[0] = pkg_name[0] = '\0';
    adjust(+1);
    assert(!test_rc("program_extdisplay"));
    assert(strcmp(jobs_double_last, "pkg-remove ffmpeg json-c") == 0 && reloads == 2);
    puts("PASS with its packages and its program line it is installed, and Enter turns it off");

    /* The program line without FFmpeg: not installed either. */
    ffmpeg_present = 0;
    rc_set("program_extdisplay", "/usr/local/bin/extdisplay");
    assert(!opt_module_installed(ext));
    puts("PASS the program line without FFmpeg is not installed either");

    /* A component with two lines needs both. */
    assert(strcmp(opt_modules[audio].probe, AUDIO_PROBE) == 0);
    audio_present = 1;
    rc_set("service_audio", AUDIO_SERVICE);
    assert(!opt_module_installed(audio));
    rc_set("program_mixer", MIXER_PROGRAM);
    assert(opt_module_installed(audio));
    puts("PASS a component with a service and a program line is installed only with both");
    return 0;
}
