/* The screens' settings (MONITORS_CONF), read by the framebuffer screen and
 * by the shared display's controller alike. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "base.h"

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

int monitor_conf(const char *name, struct monitor_conf *out)
{
    snprintf(out->place, sizeof out->place, "right");
    out->width = out->height = 0;
    out->scale = 0;
    FILE *in = fopen(MONITORS_CONF, "r");
    char line[160];
    size_t klen = strlen(name);
    int found = 0;
    while (in && !found && fgets(line, sizeof line, in)) {
        line[strcspn(line, "\r\n")] = '\0';
        if (klen == 0 || strncmp(line, name, klen) != 0 || line[klen] != '=') {
            continue;
        }
        found = 1;
        char *place = line + klen + 1, *mode = strchr(place, ','), *scale = NULL;
        if (mode) {
            *mode++ = '\0';
            scale = strchr(mode, ',');
            if (scale) {
                *scale++ = '\0';
            }
        }
        if (strcmp(place, "origin") == 0 || strcmp(place, "left") == 0 ||
            strcmp(place, "right") == 0 || strcmp(place, "above") == 0 ||
            strcmp(place, "below") == 0) {
            snprintf(out->place, sizeof out->place, "%s", place);
        }
        if (mode && sscanf(mode, "%dx%d", &out->width, &out->height) != 2) {
            out->width = out->height = 0;
        }
        double s = scale ? strtod(scale, NULL) : 0;
        if (s >= 0.5 && s <= 4.0) {
            out->scale = s;
        }
    }
    if (in) {
        fclose(in);
    }
    return found;
}
