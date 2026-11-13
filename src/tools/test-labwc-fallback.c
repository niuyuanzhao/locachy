/* seat's fallback labwc configuration (labwc_fallback_from): with no generated
 * /run/locachy/labwc-rc.xml, the template's placeholders become rc.conf's
 * defaults in labwc's spelling, so Super+Esc stays a home key in a window. */
#include <stdio.h>
#include <string.h>

#define main seat_main
#include "../progs/seat.c"
#undef main

static int failed;

static void check(int ok, const char *what)
{
    printf("%s %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok) failed = 1;
}

static int has_line(const char *path, const char *text)
{
    char line[2048];
    FILE *f = fopen(path, "re");
    int found = 0;
    if (!f) return 0;
    while (!found && fgets(line, sizeof line, f)) found = strstr(line, text) != NULL;
    fclose(f);
    return found;
}

int main(int argc, char **argv)
{
    if (argc != 2) return 2;
    char template_path[512], out_path[512];
    snprintf(template_path, sizeof template_path, "%s/template.xml", argv[1]);
    snprintf(out_path, sizeof out_path, "%s/labwc-rc.xml", argv[1]);
    FILE *t = fopen(template_path, "we");
    if (!t) return 2;
    fputs("<keyboard>\n"
          "  <keybind key=\"HOME-KEY\"><action name=\"Execute\" command=\"/usr/local/lib/locachy/labwc-control --action home\" /></keybind>\n"
          "  <keybind key=\"LOCK-KEY\"><action name=\"Execute\" command=\"/usr/local/lib/locachy/labwc-control --action lock\" /></keybind>\n"
          "  <keybind key=\"TERMINAL-KEY\"><action name=\"Execute\" command=\"foot\" /></keybind>\n"
          "  <keybind key=\"IME-KEY\"><action name=\"Execute\" command=\"ime\" /></keybind>\n"
          "</keyboard>\n", t);
    fclose(t);

    const char *rc = labwc_fallback_from(template_path, out_path);
    check(rc == out_path, "the fallback is the written file, not the template");
    check(has_line(out_path, "key=\"W-Escape\""), "Super+Esc is bound (W-Escape)");
    check(has_line(out_path, "key=\"C-A-Escape\""), "Ctrl+Alt+Esc is bound");
    check(has_line(out_path, "key=\"C-A-Super_L\""), "Ctrl+Alt+Super is bound");
    check(has_line(out_path, "key=\"XF86Launch1\""), "ThinkVantage is bound");
    check(has_line(out_path, "key=\"W-l\""), "the lock key is Super+l");
    check(has_line(out_path, "key=\"C-A-t\""), "the terminal key is Ctrl+Alt+t");
    check(has_line(out_path, "labwc-control --action home") && has_line(out_path, "--action lock"),
          "each key keeps its own action");
    check(!has_line(out_path, "HOME-KEY") && !has_line(out_path, "LOCK-KEY") && !has_line(out_path, "TERMINAL-KEY"),
          "no placeholder is left");
    check(!has_line(out_path, "IME-KEY"), "the input method placeholders are the bar's, left out");
    const char *missing = "/nonexistent/template.xml";
    check(labwc_fallback_from(missing, out_path) == missing,
          "a missing template falls back to the template path (labwc then reports it)");
    puts(failed ? "FAIL labwc fallback" : "PASS labwc fallback: Super+Esc stays home without a generated configuration");
    return failed;
}
