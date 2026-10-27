/* Settings must preserve every service and unrelated line when saving its
 * own keys: previously its fixed array silently truncated a long rc.conf.
 * test-settings-save FILE (a new scratch file, never the host rc.conf). */
#include <signal.h>

static int settings_kill(pid_t pid, int sig);
#define kill settings_kill
#define rc_set settings_rc_set
#define main settings_main
#include "../progs/settings.c"
#include "i18n-stub.h"
#undef main
#undef rc_set
#undef kill
#include <assert.h>

static const char *path;
static int reloads, writes_left = -1;

void say(const char *fmt, ...) { (void)fmt; }

int settings_rc_set(const char *name, const char *value)
{
    if (writes_left == 0) {
        return -1;
    }
    if (writes_left > 0) {
        writes_left--;
    }
    return conf_set(path, name, value);
}

static int settings_kill(pid_t pid, int sig)
{
    assert(pid == 1 && sig == SIGHUP);
    reloads++;
    return 0;
}

static int service_count, tail_seen, tailscale_seen, legacy_seen, sens_seen, bluetooth_seen;

static void value(const char *name, const char *text, void *arg)
{
    (void)arg;
    if (strncmp(name, "service_test_", 13) == 0) {
        service_count++;
    } else if (strcmp(name, "last_setting") == 0) {
        tail_seen = strcmp(text, "kept") == 0;
    } else if (strcmp(name, "service_tailscale") == 0) {
        assert(strcmp(text, TAILSCALE_SERVICE) == 0);
        tailscale_seen++;
    } else if (strcmp(name, "service_bluetooth") == 0) {
        assert(strcmp(text, BLUETOOTH_SERVICE) == 0);
        bluetooth_seen++;
    } else if (strcmp(name, "bluetooth_enabled") == 0) {
        abort(); /* only the service line represents Bluetooth being on */
    } else if (strcmp(name, "tailscale_enabled") == 0) {
        legacy_seen++;
    } else if (strcmp(name, "trackpoint_sensitivity") == 0) {
        assert(atoi(text) == tp_sens);
        sens_seen++;
    }
}

static void check_saved(int tailscale, int bluetooth)
{
    service_count = tail_seen = tailscale_seen = legacy_seen = sens_seen = bluetooth_seen = 0;
    assert(conf_read(path, value, NULL) == 0);
    assert(service_count == 160 && tail_seen && tailscale_seen == tailscale);
    assert(legacy_seen == 0 && sens_seen == 1 && bluetooth_seen == bluetooth);
}

int main(int argc, char **argv)
{
    assert(argc == 2);
    path = argv[1];
    FILE *f = fopen(path, "wx");
    assert(f);
    fputs("tailscale_enabled=\"1\"\nservice_bluetooth=\"" BLUETOOTH_SERVICE "\"\n", f);
    for (int i = 0; i < 160; i++) {
        fprintf(f, "service_test_%d=\"/bin/true %d\"\n", i, i);
    }
    fputs("last_setting=\"kept\"\n", f);
    assert(fclose(f) == 0);
    tp_present = tailscale_enabled = 1;
    assert(conf_read(path, on_rc, NULL) == 0 && bluetooth_on);
    save_rc();
    assert(reloads == 1);
    check_saved(1, 1);
    tailscale_enabled = 0;
    save_rc();
    assert(reloads == 2);
    check_saved(0, 1);
    /* General saves leave the current service line untouched, even if the
     * UI's cached Bluetooth state is stale. No duplicate enabled key. */
    bluetooth_on = 0;
    save_rc();
    assert(reloads == 3);
    check_saved(0, 1);
    assert(conf_set(path, "service_bluetooth", NULL) == 0);
    bluetooth_on = 1;
    save_rc();
    assert(reloads == 4);
    check_saved(0, 0);
    writes_left = 0;
    save_rc();
    assert(reloads == 4 && strstr(notice, "Cannot write"));
    check_saved(0, 0);
    assert(unlink(path) == 0);
    puts("settings: long rc.conf, Bluetooth service preserved on/off, service switch and failed save passed.");
    return 0;
}
