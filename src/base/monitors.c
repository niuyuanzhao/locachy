/* Shared monitor settings and connected-topology profiles. */
#include <ctype.h>
#include <dirent.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "base.h"
#include "monitor-profile.h"

static void on_internal(const char *name, const char *value, void *arg)
{
    if (strcmp(name, "internal_screen") == 0) snprintf(arg, 32, "%s", value);
}

int monitor_internal(const char *name)
{
    char named[32] = "";
    rc_read(on_internal, named);
    if (named[0]) return strcmp(name, named) == 0;
    return strncmp(name, "eDP", 3) == 0 || strncmp(name, "LVDS", 4) == 0 || strncmp(name, "DSI", 3) == 0;
}

int monitor_conf_parse(const char *value, struct monitor_conf *out)
{
    memset(out, 0, sizeof *out);
    snprintf(out->place, sizeof out->place, "right");
    char text[192], *save = NULL;
    snprintf(text, sizeof text, "%s", value);
    text[strcspn(text, "\r\n")] = '\0';
    char *place = strtok_r(text, ",", &save), *mode = strtok_r(NULL, ",", &save);
    char *scale = strtok_r(NULL, ",", &save), *x = strtok_r(NULL, ",", &save), *y = strtok_r(NULL, ",", &save);
    if (!place) return 0;
    if (!strcmp(place, "origin") || !strcmp(place, "left") || !strcmp(place, "right") ||
        !strcmp(place, "above") || !strcmp(place, "below")) snprintf(out->place, sizeof out->place, "%s", place);
    if (mode) {
        int end = 0;
        if (sscanf(mode, "%dx%d%n", &out->width, &out->height, &end) == 2 && out->width > 0 && out->height > 0) {
            if (mode[end] == '@') {
                char *last;
                long refresh = strtol(mode + end + 1, &last, 10);
                if (*last || refresh < 1 || refresh > 1000000) out->width = out->height = 0;
                else out->refresh_mhz = (int)refresh;
            } else if (mode[end]) out->width = out->height = 0;
        } else out->width = out->height = 0;
    }
    double s = scale ? strtod(scale, NULL) : 0;
    if (s >= .5 && s <= 4) out->scale = s;
    int px, py, ex = 0, ey = 0;
    if (x && y && sscanf(x, "%d%n", &px, &ex) == 1 && !x[ex] && sscanf(y, "%d%n", &py, &ey) == 1 && !y[ey] &&
        px >= -65536 && px <= 65536 && py >= -65536 && py <= 65536) {
        out->has_position = 1; out->x = px; out->y = py;
    }
    return 1;
}

int monitor_conf_format(char *out, size_t size, const struct monitor_conf *c)
{
    char mode[64] = "preferred", position[48] = "";
    if (c->width > 0 && c->height > 0) {
        snprintf(mode, sizeof mode, "%dx%d", c->width, c->height);
        if (c->refresh_mhz > 0) snprintf(mode + strlen(mode), sizeof mode - strlen(mode), "@%d", c->refresh_mhz);
    }
    if (c->has_position) snprintf(position, sizeof position, ",%d,%d", c->x, c->y);
    int n = snprintf(out, size, "%s,%s,%g%s", c->place, mode, c->scale > 0 ? c->scale : 1, position);
    return n >= 0 && (size_t)n < size;
}

/* Two independently seeded 64-bit rolling hashes identify bytes, not an EDID
 * interpretation. This is matching metadata, not a security signature. */
static void hash_bytes(const unsigned char *data, size_t n, uint64_t *a, uint64_t *b)
{
    for (size_t i = 0; i < n; i++) { *a = (*a ^ data[i]) * UINT64_C(1099511628211); *b = (*b ^ data[i]) * UINT64_C(14029467366897019727); }
}

void monitor_identity_sysfs(const char *connector, struct monitor_identity *out)
{
    memset(out, 0, sizeof *out);
    snprintf(out->connector, sizeof out->connector, "%s", connector);
    snprintf(out->fingerprint, sizeof out->fingerprint, "connector-%.54s", connector);
    DIR *d = opendir("/sys/class/drm");
    struct dirent *e;
    char selected[256] = "";
    while (d && (e = readdir(d))) {
        const char *dash = strchr(e->d_name, '-');
        if (strncmp(e->d_name, "card", 4) || !dash || strcmp(dash + 1, connector)) continue;
        if (!selected[0] || strcmp(e->d_name, selected) < 0) snprintf(selected, sizeof selected, "%s", e->d_name);
    }
    if (d) closedir(d);
    if (!selected[0]) return;
    char path[320];
    snprintf(path, sizeof path, "/sys/class/drm/%s/edid", selected);
    FILE *f = fopen(path, "rb");
    uint64_t a = UINT64_C(14695981039346656037), b = UINT64_C(7809847782465536322);
    unsigned char data[512]; size_t n, total = 0;
    while (f && total < 32768 && (n = fread(data, 1, sizeof data, f)) > 0) { hash_bytes(data, n, &a, &b); total += n; }
    int good = f && !ferror(f) && total > 0 && total < 32768;
    if (f) fclose(f);
    if (good) snprintf(out->fingerprint, sizeof out->fingerprint, "edid-%016llx%016llx", (unsigned long long)a, (unsigned long long)b);
}

static struct monitor_identity active_ids[MONITOR_PROFILE_MAX];
static struct monitor_conf active_conf[MONITOR_PROFILE_MAX];
static int active_n, active_match;

static void identity_key(const struct monitor_identity *ids, int n, int i, char *out, size_t size)
{
    int duplicate = 0;
    for (int j = 0; j < n; j++) if (i != j && !strcmp(ids[i].fingerprint, ids[j].fingerprint)) duplicate = 1;
    snprintf(out, size, "%s%s%s", ids[i].fingerprint, duplicate ? "@" : "", duplicate ? ids[i].connector : "");
}

static int topology_key(const struct monitor_identity *ids, int n, char *out, size_t size)
{
    if (n < 1 || n > MONITOR_PROFILE_MAX) return 0;
    char keys[MONITOR_PROFILE_MAX][144], tmp[144];
    for (int i = 0; i < n; i++) identity_key(ids, n, i, keys[i], sizeof keys[i]);
    for (int i = 0; i < n; i++) for (int j = i + 1; j < n; j++) if (strcmp(keys[i], keys[j]) > 0) {
        memcpy(tmp, keys[i], sizeof tmp); memcpy(keys[i], keys[j], sizeof tmp); memcpy(keys[j], tmp, sizeof tmp);
    }
    uint64_t a = UINT64_C(14695981039346656037), b = UINT64_C(7809847782465536322);
    for (int i = 0; i < n; i++) hash_bytes((unsigned char *)keys[i], strlen(keys[i]) + 1, &a, &b);
    snprintf(out, size, "%016llx%016llx", (unsigned long long)a, (unsigned long long)b);
    return 1;
}

int monitor_profiles_select(const struct monitor_identity *ids, int n)
{
    active_match = active_n = 0;
    char wanted[40];
    if (!topology_key(ids, n, wanted, sizeof wanted)) return 0;
    memcpy(active_ids, ids, (size_t)n * sizeof *ids); active_n = n;
    FILE *f = fopen(MONITOR_PROFILES_CONF, "r");
    char line[512]; int inside = 0, found[MONITOR_PROFILE_MAX] = {0};
    while (f && fgets(line, sizeof line, f)) {
        line[strcspn(line, "\r\n")] = '\0';
        if (!strncmp(line, "profile=", 8)) { inside = !strcmp(line + 8, wanted); memset(found, 0, sizeof found); }
        else if (!strcmp(line, "end")) {
            int count = 0; for (int i = 0; i < n; i++) count += found[i];
            if (inside && count == n) { active_match = 1; break; }
            inside = 0;
        } else if (inside) {
            char *tab = strchr(line, '\t'); if (!tab) continue; *tab++ = '\0';
            char *value = strchr(tab, '\t'); if (!value) continue; *value++ = '\0';
            for (int i = 0; i < n; i++) {
                char key[144]; identity_key(ids, n, i, key, sizeof key);
                if (!strcmp(key, line) && monitor_conf_parse(value, &active_conf[i])) found[i] = 1;
            }
        }
    }
    if (f) fclose(f);
    return active_match;
}

int monitor_profiles_connected(struct monitor_identity *ids, int max)
{
    int n = 0;
    DIR *d = opendir("/sys/class/drm"); struct dirent *e;
    while (d && n < max && (e = readdir(d))) {
        const char *dash = strchr(e->d_name, '-');
        if (strncmp(e->d_name, "card", 4) || !dash) continue;
        char path[320], status[32] = "";
        snprintf(path, sizeof path, "/sys/class/drm/%s/status", e->d_name);
        FILE *f = fopen(path, "r"); if (f) { if (!fgets(status, sizeof status, f)) status[0] = '\0'; fclose(f); }
        if (strncmp(status, "connected", 9)) continue;
        monitor_identity_sysfs(dash + 1, &ids[n++]);
    }
    if (d) closedir(d);
    return n;
}

int monitor_profiles_select_sysfs(void)
{
    struct monitor_identity ids[MONITOR_PROFILE_MAX];
    int n = monitor_profiles_connected(ids, MONITOR_PROFILE_MAX);
    return monitor_profiles_select(ids, n);
}

int monitor_profiles_matched(void) { return active_match; }

int monitor_conf(const char *name, struct monitor_conf *out)
{
    if (active_match) for (int i = 0; i < active_n; i++) if (!strcmp(name, active_ids[i].connector)) { *out = active_conf[i]; return 1; }
    monitor_conf_parse("right,preferred,0", out);
    FILE *f = fopen(MONITORS_CONF, "r"); char line[256]; size_t len = strlen(name); int found = 0;
    while (f && !found && fgets(line, sizeof line, f)) if (len && !strncmp(line, name, len) && line[len] == '=') found = monitor_conf_parse(line + len + 1, out);
    if (f) fclose(f);
    return found;
}

int monitor_profiles_save(const struct monitor_profile_entry *entries, int n)
{
    struct monitor_identity ids[MONITOR_PROFILE_MAX]; char key[40];
    if (n < 1 || n > MONITOR_PROFILE_MAX) return -1;
    for (int i = 0; i < n; i++) ids[i] = entries[i].identity;
    if (!topology_key(ids, n, key, sizeof key)) return -1;
    FILE *in = fopen(MONITOR_PROFILES_CONF, "r"), *out = fopen(MONITOR_PROFILES_CONF ".tmp", "w");
    if (!out) { if (in) fclose(in); return -1; }
    char line[512]; int skip = 0, failed = 0; size_t bytes = 0;
    while (in && fgets(line, sizeof line, in)) {
        bytes += strlen(line); if (bytes > 256 * 1024) { failed = 1; break; }
        if (!strncmp(line, "profile=", 8)) { char *end = strpbrk(line, "\r\n"); if (end) *end = '\0'; skip = !strcmp(line + 8, key); if (end) *end = '\n'; }
        if (!skip && fputs(line, out) == EOF) failed = 1;
        if (!strncmp(line, "end", 3)) skip = 0;
    }
    if (in && ferror(in)) failed = 1;
    if (in) fclose(in);
    failed |= fprintf(out, "profile=%s\n", key) < 0;
    for (int i = 0; i < n; i++) {
        char id[144], value[192]; identity_key(ids, n, i, id, sizeof id);
        if (!monitor_conf_format(value, sizeof value, &entries[i].conf)) failed = 1;
        failed |= fprintf(out, "%s\t%s\t%s\n", id, ids[i].connector, value) < 0;
    }
    failed |= fputs("end\n", out) == EOF;
    if (fflush(out) || fsync(fileno(out))) failed = 1;
    if (fclose(out)) failed = 1;
    if (!failed && rename(MONITOR_PROFILES_CONF ".tmp", MONITOR_PROFILES_CONF) == 0) { monitor_profiles_select(ids, n); return 0; }
    unlink(MONITOR_PROFILES_CONF ".tmp");
    return -1;
}
