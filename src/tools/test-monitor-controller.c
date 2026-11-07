/* Controller callbacks and exact mode choice without a compositor. */
#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <wayland-client.h>
#include "wlr-output-management-unstable-v1-client-protocol.h"
static int destroyed;
static void fixture_destroy(struct zwlr_output_configuration_v1 *c) { (void)c; destroyed++; }
#define zwlr_output_configuration_v1_destroy fixture_destroy
#define main controller_main
#include "../progs/labwc-control.c"
#undef main
#undef zwlr_output_configuration_v1_destroy
void say(const char *fmt, ...) { (void)fmt; }
double uptime(void) { return 100; }
int main(void)
{
    struct mode low = { .width=1920,.height=1080,.refresh=30000 }, high = { .width=1920,.height=1080,.refresh=60000,.next=&low };
    struct head h={ .modes=&high,.current=&low };
    struct monitor_conf wanted={.width=1920,.height=1080};
    assert(pick_mode(&h,&wanted)==&low);
    wanted.refresh_mhz=60000;assert(pick_mode(&h,&wanted)==&high);
    wanted.refresh_mhz=59940;assert(pick_mode(&h,&wanted)==&low);
    struct monitor_conf automatic={0};high.preferred=1;
    assert(pick_mode(&h,&automatic)==&low); /* automatic never undoes a usable active rate */
    puts("PASS controller keeps current legacy rate, selects exact mHz, and retains current if request unavailable");
    heads_changed=0;head_enabled(&h,NULL,1);assert(heads_changed&&h.enabled);
    heads_changed=0;head_enabled(&h,NULL,1);assert(!heads_changed);
    mode_size(&low,NULL,1280,720);assert(heads_changed);
    heads_changed=0;mode_refresh(&low,NULL,25000);assert(heads_changed);
    puts("PASS a late enabled head and new mode capabilities mark layout selection dirty");
    for (int applying=0; applying<2; applying++) {
        struct monitor_transaction *t=calloc(1,sizeof *t); assert(t); t->applying=applying;
        pending_monitors=t;apply_at=0;reload_wanted=0;
        config_failed(t,(void *)1);
        assert(!pending_monitors && apply_at==0 && h.current==&low);
    }
    puts("PASS failed test or commit leaves active mode unchanged with no identical retry loop");
    struct monitor_transaction *t=calloc(1,sizeof *t);pending_monitors=t;
    output_serial=9;t->serial=8;heads_changed=0;reload_wanted=0;apply_at=0;
    config_succeeded(t,(void *)1);assert(!pending_monitors&&reload_wanted&&apply_at>0);
    t=calloc(1,sizeof *t);pending_monitors=t;reload_wanted=0;apply_at=0;
    config_cancelled(t,(void *)1);assert(!pending_monitors&&reload_wanted&&apply_at>0);
    t=calloc(1,sizeof *t);pending_monitors=t;t->serial=output_serial;
    t->heads[0]=(struct head *)1;t->modes[0]=(struct mode *)1;t->n=1;
    heads_changed=1;reload_wanted=0;apply_at=0;
    config_succeeded(t,(void *)1);assert(!pending_monitors&&reload_wanted&&apply_at>0);
    assert(destroyed==5);
    puts("PASS stale tested topology and compositor cancellation discard pending state and reselect on an event deadline");
    return 0;
}
