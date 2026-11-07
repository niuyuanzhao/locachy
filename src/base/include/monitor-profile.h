/* Saved layouts are selected by the whole connected display set. */
#ifndef LOCACHY_MONITOR_PROFILE_H
#define LOCACHY_MONITOR_PROFILE_H
#include "base.h"
#ifndef MONITOR_PROFILES_CONF
#define MONITOR_PROFILES_CONF "/etc/locachy/monitor-profiles.conf"
#endif
#define MONITOR_PROFILE_MAX 32
struct monitor_identity {
    char connector[64];
    char fingerprint[65]; /* raw EDID hash, else connector identity */
};
struct monitor_profile_entry {
    struct monitor_identity identity;
    struct monitor_conf conf;
};
/* The same host sysfs identity is used by settings, KMS and the controller.
 * No EDID interpretation: only the kernel-provided bytes are hashed. */
void monitor_identity_sysfs(const char *connector, struct monitor_identity *out);
int monitor_profiles_select(const struct monitor_identity *connected, int n);
int monitor_profiles_select_sysfs(void);
int monitor_profiles_connected(struct monitor_identity *out, int max);
int monitor_profiles_save(const struct monitor_profile_entry *entries, int n);
int monitor_profiles_matched(void);
/* Parser/formatter shared with settings and the profile store. */
int monitor_conf_parse(const char *value, struct monitor_conf *out);
int monitor_conf_format(char *out, size_t size, const struct monitor_conf *conf);
#endif
