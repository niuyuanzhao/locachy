/* Tor Browser alone has a supported bundled-Tor launch. Shared by the
 * confirmation UIs; locachy-app independently enforces the same condition. */
#ifndef LOCACHY_TOR_BROWSER_H
#define LOCACHY_TOR_BROWSER_H
#include <stdio.h>
#include <string.h>

static inline int tor_browser_own_supported(const char *path)
{
    char line[16386], package[64] = "", command[128] = "", arguments[16386] = "";
    int network = 0, tor = 0;
    FILE *f = fopen(path, "re");
    if (!f) return 0;
    while (fgets(line, sizeof line, f)) {
        if (!strchr(line, '\n') && !feof(f)) {
            fclose(f);
            return 0;
        }
        char *key = line + strspn(line, " \t"), *eq = strchr(key, '=');
        if (!eq || eq[1] != '"') continue;
        char *value = eq + 2, *end = strchr(value, '"');
        if (!end) continue;
        *eq = *end = '\0';
        if (!strcmp(key, "package")) snprintf(package, sizeof package, "%s", value);
        else if (!strcmp(key, "command")) snprintf(command, sizeof command, "%s", value);
        else if (!strcmp(key, "arguments")) snprintf(arguments, sizeof arguments, "%s", value);
        else if (!strcmp(key, "network")) network = !strcmp(value, "1");
        else if (!strcmp(key, "tor")) tor = !strcmp(value, "1");
    }
    fclose(f);
    if (!network || !tor || strcmp(package, "tor-browser-bin")) return 0;
    if (!strcmp(command, "/usr/bin/tor-browser")) return 1;
    if (strcmp(command, "/usr/bin/env")) return 0;
    char *save = NULL, *word = strtok_r(arguments, " \t", &save);
    if (!word || strcmp(word, "MOZ_APP_REMOTINGNAME=tor-browser")) return 0;
    word = strtok_r(NULL, " \t", &save);
    if (word && !strcmp(word, "TOR_PROVIDER=none")) {
        /* The exact prefix of the previously shipped system-Tor recipe. */
        word = strtok_r(NULL, " \t", &save);
        if (!word || strcmp(word, "TOR_SOCKS_HOST=127.0.0.1")) return 0;
        word = strtok_r(NULL, " \t", &save);
        if (!word || strcmp(word, "TOR_SOCKS_PORT=9050")) return 0;
        word = strtok_r(NULL, " \t", &save);
    }
    return word && !strcmp(word, "/usr/bin/tor-browser");
}
#endif
