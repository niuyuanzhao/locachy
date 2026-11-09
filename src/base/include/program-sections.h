/* The host launchers' sections, shared by home and the desktop launcher. */
#ifndef LOCACHY_PROGRAM_SECTIONS_H
#define LOCACHY_PROGRAM_SECTIONS_H

#include <stddef.h>
#include <string.h>

enum section { SECTION_CONTAINERS, SECTION_PROGRAMS, SECTION_SYSTEM, SECTIONS };

/* Unknown program_NAME launchers belong to Programs. Within each section
 * rc.conf's order stays; files is always the first program. */
static inline enum section program_section(const char *name)
{
    static const char *const system[] = {
        "admin", "network", "settings", "packages", "firmware", "bluetooth", "mixer"
    };
    for (size_t i = 0; i < sizeof system / sizeof *system; i++) {
        if (strcmp(name, system[i]) == 0) return SECTION_SYSTEM;
    }
    return SECTION_PROGRAMS;
}

/* files_program_request carries these only with an empty target. A colon
 * cannot occur in an ordinary program name, so no program_NAME can collide. */
#define HOME_REQUEST_REBOOT ":home:reboot"
#define HOME_REQUEST_POWEROFF ":home:poweroff"

#endif
