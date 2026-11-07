/* Actual profile persistence on private files, no devices or root. */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <assert.h>
#include <dirent.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
static char root[1024];
static FILE *fixture_fopen(const char *path, const char *mode);
static DIR *fixture_opendir(const char *path);
static int fixture_rename(const char *from, const char *to);
static int fixture_unlink(const char *path);
#include <dirent.h>
#define fopen fixture_fopen
#define opendir fixture_opendir
#define rename fixture_rename
#define unlink fixture_unlink
#include "../base/monitors.c"
#undef unlink
#undef rename
#undef opendir
#undef fopen
static int fail_rename;
int rc_read(void (*fn)(const char *, const char *, void *), void *arg) { (void)fn; (void)arg; return 0; }
static const char *mapped(const char *path, char *out)
{
    if (!strncmp(path, "/sys/class/drm", 14)) snprintf(out, 1400, "%s/drm%s", root, path + 14);
    else if (!strncmp(path, "/etc/locachy/", 13)) snprintf(out, 1400, "%s/%s", root, path + 13);
    else snprintf(out, 1400, "%s", path);
    return out;
}
static FILE *fixture_fopen(const char *p, const char *m) { char out[1400]; return fopen(mapped(p,out),m); }
static DIR *fixture_opendir(const char *p) { char out[1400]; return opendir(mapped(p,out)); }
static int fixture_rename(const char *a, const char *b) { char x[1400],y[1400]; return fail_rename ? -1 : rename(mapped(a,x),mapped(b,y)); }
static int fixture_unlink(const char *p) { char out[1400]; return unlink(mapped(p,out)); }
static void put(const char *name, const char *text)
{
    char p[1400]; snprintf(p,sizeof p,"%s/%s",root,name); FILE *f=fopen(p,"w"); assert(f); assert(fputs(text,f)>=0); assert(!fclose(f));
}
static void connector(const char *name, const char *status, const char *edid)
{
    char p[1400],rel[256]; snprintf(p,sizeof p,"%s/drm/card0-%s",root,name); mkdir(p,0700);
    snprintf(rel,sizeof rel,"drm/card0-%s/status",name);put(rel,status);
    snprintf(rel,sizeof rel,"drm/card0-%s/edid",name);put(rel,edid);
}
static struct monitor_profile_entry entry(const char *name, const char *value)
{
    struct monitor_profile_entry e; monitor_identity_sysfs(name,&e.identity); monitor_conf_parse(value,&e.conf); return e;
}
int main(int argc,char **argv)
{
    assert(argc==2); snprintf(root,sizeof root,"%s",argv[1]); assert(root[0]=='/'); assert(!mkdir(root,0700));
    char p[1400];snprintf(p,sizeof p,"%s/drm",root);assert(!mkdir(p,0700));
    connector("eDP-1","connected\n","internal bytes"); connector("HDMI-A-1","connected\n","external bytes");
    put("monitors.conf","eDP-1=origin,1366x768,1\nHDMI-A-1=left,1920x1080,1,-1920,0\n");
    assert(!monitor_profiles_select_sysfs()); struct monitor_conf c;
    assert(monitor_conf("HDMI-A-1",&c) && !strcmp(c.place,"left") && c.x==-1920);
    struct monitor_profile_entry dual[]={entry("eDP-1","origin,1366x768@60000,1,0,0"),entry("HDMI-A-1","left,1920x1080@30000,1.25,-1536,-90")};
    assert(!monitor_profiles_save(dual,2)); assert(monitor_profiles_select_sysfs());
    assert(monitor_conf("HDMI-A-1",&c)&&c.refresh_mhz==30000&&c.x==-1536&&c.y==-90&&c.scale==1.25);
    connector("eDP-1","disconnected\n","internal bytes"); assert(!monitor_profiles_select_sysfs());
    struct monitor_profile_entry single[]={entry("HDMI-A-1","origin,1920x1080@60000,1.5,0,0")};
    assert(!monitor_profiles_save(single,1));assert(monitor_profiles_select_sysfs());
    connector("eDP-1","connected\n","internal bytes"); assert(monitor_profiles_select_sysfs());
    assert(monitor_conf("HDMI-A-1",&c)&&c.refresh_mhz==30000&&c.x==-1536&&c.scale==1.25);
    puts("PASS dual and external-only profiles restore independently; legacy fallback is not overwritten");
    struct monitor_identity reversed[]={dual[1].identity,dual[0].identity}; assert(monitor_profiles_select(reversed,2));
    fail_rename=1;dual[1].conf.x=-700;assert(monitor_profiles_save(dual,2)<0);fail_rename=0;
    assert(monitor_profiles_select(reversed,2));assert(monitor_conf("HDMI-A-1",&c)&&c.x==-1536);
    puts("PASS discovery order is irrelevant; failed atomic save retains previous profile");
    connector("HDMI-A-1","disconnected\n","external bytes");connector("DP-2","connected\n","external bytes");
    assert(monitor_profiles_select_sysfs());assert(monitor_conf("DP-2",&c)&&c.x==-1536&&c.refresh_mhz==30000);
    connector("DP-2","connected\n","another display same connector");assert(!monitor_profiles_select_sysfs());
    puts("PASS raw EDID identity follows port changes and distinguishes another display on the same port");
    connector("eDP-1","disconnected\n","");connector("DP-2","connected\n","");
    assert(!monitor_profiles_select_sysfs());struct monitor_profile_entry absent[]={entry("DP-2","origin,1280x720,1")};
    assert(!monitor_profiles_save(absent,1));assert(monitor_profiles_select_sysfs());
    puts("PASS missing EDID uses shared connector fallback without interpreting EDID");
    connector("DP-3","connected\n","");assert(!monitor_profiles_select_sysfs());
    struct monitor_profile_entry copies[]={entry("DP-2","origin,1280x720,1"),entry("DP-3","right,1280x720,1,1280,0")};
    snprintf(copies[0].identity.fingerprint,sizeof copies[0].identity.fingerprint,"identical-edid");
    snprintf(copies[1].identity.fingerprint,sizeof copies[1].identity.fingerprint,"identical-edid");
    assert(!monitor_profiles_save(copies,2));struct monitor_identity copy_ids[]={copies[1].identity,copies[0].identity};
    assert(monitor_profiles_select(copy_ids,2));assert(monitor_conf("DP-3",&c)&&c.x==1280);
    puts("PASS identical identities are disambiguated by connector within the connected set");
    monitor_conf_parse("left,1920x1080@59940,1.25,-1536,-80",&c);assert(c.refresh_mhz==59940&&c.has_position);
    char value[192];assert(monitor_conf_format(value,sizeof value,&c));assert(strstr(value,"@59940"));
    puts("PASS exact millihertz, scale and signed positions round-trip");
    /* A truncated record never becomes an active partial topology. */
    put("monitor-profiles.conf", "profile=invalid\npartial\n");
    assert(!monitor_profiles_select(copy_ids,2));
    assert(monitor_conf("HDMI-A-1",&c)&&c.x==-1920);
    puts("PASS incomplete profile records fall back to untouched legacy layout");
    return 0;
}
