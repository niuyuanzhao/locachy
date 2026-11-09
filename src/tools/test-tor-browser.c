/* The UI's bundled-Tor eligibility must match the supported package wrapper. */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include "tor-browser.h"

static void put(const char *path, const char *text)
{
    FILE *f = fopen(path, "w");
    assert(f && fputs(text, f) >= 0 && fclose(f) == 0);
}

#define COMMON "network=\"1\"\ntor=\"1\"\npackage=\"tor-browser-bin\"\n"
int main(void)
{
    char path[] = "/tmp/tor-browser-conf-XXXXXX";
    int fd = mkstemp(path);
    assert(fd >= 0 && close(fd) == 0);
    put(path, COMMON "command=\"/usr/bin/tor-browser\"\n");
    assert(tor_browser_own_supported(path));
    put(path, COMMON "command=\"/usr/bin/env\"\n"
        "arguments=\"MOZ_APP_REMOTINGNAME=tor-browser /usr/bin/tor-browser --private-window\"\n");
    assert(tor_browser_own_supported(path));
    put(path, COMMON "command=\"/usr/bin/env\"\n"
        "arguments=\"MOZ_APP_REMOTINGNAME=tor-browser TOR_PROVIDER=none TOR_SOCKS_HOST=127.0.0.1 "
        "TOR_SOCKS_PORT=9050 /usr/bin/tor-browser\"\n");
    assert(tor_browser_own_supported(path));
    puts("PASS bundled Tor UI accepts the direct, current and exact legacy Tor Browser wrapper");

    put(path, COMMON "command=\"/bin/true\"\n");
    assert(!tor_browser_own_supported(path));
    put(path, COMMON "command=\"/usr/bin/tor-browser\"\npackage=\"firefox\"\n");
    assert(!tor_browser_own_supported(path));
    put(path, COMMON "command=\"/usr/bin/tor-browser\"\nnetwork=\"0\"\n");
    assert(!tor_browser_own_supported(path));
    put(path, COMMON "command=\"/usr/bin/tor-browser\"\ntor=\"0\"\n");
    assert(!tor_browser_own_supported(path));
    put(path, COMMON "command=\"/usr/bin/env\"\n"
        "arguments=\"MOZ_APP_REMOTINGNAME=tor-browser TOR_PROVIDER=none TOR_SOCKS_HOST=127.0.0.1 "
        "TOR_SOCKS_PORT=9150 /usr/bin/tor-browser\"\n");
    assert(!tor_browser_own_supported(path));
    put(path, COMMON "command=\"/usr/bin/env\"\n"
        "arguments=\"OTHER=1 /usr/bin/tor-browser\"\n");
    assert(!tor_browser_own_supported(path));
    assert(unlink(path) == 0);
    assert(!tor_browser_own_supported(path));
    puts("PASS generic Tor-only apps, custom wrappers, disabled networking and duplicate-key overrides cannot offer own Tor");
    return 0;
}
