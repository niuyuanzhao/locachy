/* Battery status from temporary power_supply fixtures, or --live read-only. */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static const char *supply_dir;
#define POWER_SUPPLY_DIR supply_dir
#include "../base/status.c"
#include "i18n-stub.h"

double uptime(void) { return 0; }
int rc_read(void (*fn)(const char *, const char *, void *), void *arg)
{ (void)fn; (void)arg; return 0; }

static char fixture[1024];

static void scenario(const char *root, const char *name)
{
    int n = snprintf(fixture, sizeof fixture, "%s/%s", root, name);
    assert(n > 0 && (size_t)n < sizeof fixture);
    assert(mkdir(fixture, 0700) == 0);
    supply_dir = fixture;
}

static void put(const char *name, const char *attribute, const char *value)
{
    char path[1400];
    int n = snprintf(path, sizeof path, "%s/%s", supply_dir, name);
    assert(n > 0 && (size_t)n < sizeof path);
    assert(mkdir(path, 0700) == 0 || errno == EEXIST);
    n = snprintf(path, sizeof path, "%s/%s/%s", supply_dir, name, attribute);
    assert(n > 0 && (size_t)n < sizeof path);
    FILE *f = fopen(path, "w");
    assert(f && fprintf(f, "%s\n", value) > 0 && fclose(f) == 0);
}

static void check(const char *battery, const char *power)
{
    update_battery();
    if (strcmp(fields.battery, battery) || strcmp(fields.power, power))
        fprintf(stderr, "got %s | %s, expected %s | %s\n", fields.battery, fields.power, battery, power);
    assert(strcmp(fields.battery, battery) == 0);
    assert(strcmp(fields.power, power) == 0);
}

int main(int argc, char **argv)
{
    assert(argc == 2);
    if (!strcmp(argv[1], "--live")) {
        supply_dir = "/sys/class/power_supply";
        update_battery();
        printf("%s\n%s | %s\n", cached_bat, fields.battery, fields.power);
        return 0;
    }
    const char *root = argv[1];
    assert(mkdir(root, 0700) == 0);
    scenario(root, "bat1-ac");
    put("BAT1", "type", "Battery");
    put("BAT1", "capacity", "99");
    put("BAT1", "status", "Not charging");
    put("AC", "type", "Mains");
    put("AC", "online", "1");
    check("Bat 99%", "AC");
    puts("PASS BAT1 works without BAT0");

    scenario(root, "usb-charging");
    put("CMB2", "type", "Battery");
    put("CMB2", "capacity", "42");
    put("CMB2", "status", "Charging");
    put("CMB2", "power_now", "12500000");
    put("ucsi-source-psy-USBC000:001", "type", "USB_PD");
    put("ucsi-source-psy-USBC000:001", "online", "1");
    check("Bat 42%", "charging, 12.5 W");
    puts("PASS USB-C power and arbitrary supply names");

    scenario(root, "discharging");
    put("BAT0", "type", "Battery");
    put("BAT0", "capacity", "50");
    put("BAT0", "status", "Discharging");
    put("BAT0", "energy_now", "20000000");
    put("BAT0", "energy_full", "40000000");
    put("BAT0", "power_now", "5000000");
    check("Bat 50%", "5.0 W, 4h00m");
    puts("PASS existing watt and runtime display preserved");

    scenario(root, "charge-units");
    put("BAT2", "type", "Battery");
    put("BAT2", "charge_now", "2000000");
    put("BAT2", "charge_full", "4000000");
    put("BAT2", "voltage_min_design", "10000000");
    put("BAT2", "voltage_now", "10000000");
    put("BAT2", "current_now", "500000");
    check("Bat 50%", "5.0 W, 4h00m");
    puts("PASS charge/current drivers converted to energy/power");

    scenario(root, "two-batteries");
    put("BAT0", "type", "Battery");
    put("BAT0", "capacity", "50");
    put("BAT0", "energy_now", "20000000");
    put("BAT0", "energy_full", "40000000");
    put("BAT0", "power_now", "5000000");
    put("BAT1", "type", "Battery");
    put("BAT1", "capacity", "100");
    put("BAT1", "energy_now", "60000000");
    put("BAT1", "energy_full", "60000000");
    put("BAT1", "power_now", "5000000");
    check("Bat 80%", "10.0 W, 8h00m");
    puts("PASS multiple batteries weighted by energy, not first node");

    scenario(root, "peripheral");
    put("BAT1", "type", "Battery");
    put("BAT1", "capacity", "60");
    put("mouse-battery", "type", "Battery");
    put("mouse-battery", "scope", "Device");
    put("mouse-battery", "capacity", "5");
    put("BAT2", "type", "Battery");
    put("BAT2", "present", "0");
    put("BAT2", "capacity", "0");
    check("Bat 60%", "");
    puts("PASS absent and peripheral batteries excluded");

    scenario(root, "full-no-mains");
    put("pack", "type", "Battery");
    put("pack", "capacity", "100");
    put("pack", "status", "Full");
    check("Bat 100%", "AC, full");
    puts("PASS status fallback when no online attribute exists");

    scenario(root, "no-battery");
    put("ADP1", "type", "Mains");
    put("ADP1", "online", "1");
    check("AC on", "");
    put("ADP1", "online", "0");
    check("AC off", "");
    puts("PASS desktop AC-only state and unplug refresh");

    scenario(root, "malformed");
    put("BAT1", "type", "Battery");
    put("BAT1", "capacity", "garbage");
    put("BAT1", "power_now", "-1");
    check("AC off", "");
    put("BAT1", "capacity", "65");
    check("Bat 65%", "");
    puts("PASS invalid values do not become zero capacity; later reads recover");
    puts("ALL PASSED");
    return 0;
}
