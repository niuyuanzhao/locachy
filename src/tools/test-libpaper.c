/* Installed libpaper lifecycle: repeat initialization, release all state,
 * then change XDG ownership. Run normally and under ASan/LeakSanitizer. */
#define _GNU_SOURCE
#include <assert.h>
#include <paper.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

int main(int argc, char **argv)
{
    assert(argc == 2);
    const char *dir = argv[1];
    assert(mkdir(dir, 0700) == 0);
    char home[4096], config[4096], file[4096];
    assert(snprintf(home, sizeof home, "%s/home", dir) < (int)sizeof home);
    assert(snprintf(config, sizeof config, "%s/config", dir) < (int)sizeof config);
    assert(mkdir(home, 0700) == 0 && mkdir(config, 0700) == 0);
    assert(snprintf(file, sizeof file, "%s/paperspecs", config) < (int)sizeof file);
    FILE *f = fopen(file, "w");
    assert(f && fputs("locachy-test,123,456,mm\n", f) >= 0 && fclose(f) == 0);
    assert(setenv("HOME", home, 1) == 0);
    assert(unsetenv("XDG_CONFIG_HOME") == 0);
    for (int i = 0; i < 24; i++) {
        assert(paperinit() == PAPER_OK);
        assert(defaultpapername());
        assert(paperfirst());
        assert(paperdone() == PAPER_OK);
        assert(paperdone() == PAPER_OK);
    }
    puts("PASS libpaper repeated init/default/done and idempotent done");
    assert(setenv("XDG_CONFIG_HOME", config, 1) == 0);
    assert(paperinit() == PAPER_OK);
    const struct paper *custom = paperinfo("locachy-test");
    assert(custom && strcmp(papername(custom), "locachy-test") == 0);
    assert(paperwidth(custom) == 123 && paperheight(custom) == 456);
    assert(paperdone() == PAPER_OK);
    puts("PASS libpaper owned HOME path can change to borrowed XDG path without invalid free");
    assert(unsetenv("XDG_CONFIG_HOME") == 0);
    assert(paperinit() == PAPER_OK);
    assert(!paperinfo("locachy-test"));
    assert(paperfirst() && paperdone() == PAPER_OK);
    puts("PASS libpaper fresh initialization drops old user catalog and preserves system sizes");
    assert(unlink(file) == 0 && rmdir(config) == 0 && rmdir(home) == 0 && rmdir(dir) == 0);
    return 0;
}
