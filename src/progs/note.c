/*
 * note NAME MIB: a test program for switching. It fills MIB MiB with a
 * pattern made from its name, and keeps the lines typed into it. Each time
 * it is given the screen (first, and after each switch back) it checks the
 * whole pattern, so a run shows that its memory came back from swap intact
 * and that it is the same process, not a new one.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "base.h"

#define FG    0xE0D8C8
#define BG    0x181410
#define MAX_LINES 8

static uint64_t *mem;
static size_t words;
static uint64_t seed;

static uint64_t word(size_t i)
{
    return seed ^ (i * 0x9E3779B97F4A7C15ull);
}

/* The index of the first word that is wrong, or words if none is. */
static size_t check(void)
{
    for (size_t i = 0; i < words; i++) {
        if (mem[i] != word(i)) {
            return i;
        }
    }
    return words;
}

int main(int argc, char **argv)
{
    const char *name = argc > 1 ? argv[1] : "note";
    size_t mib = argc > 2 ? strtoul(argv[2], NULL, 10) : 16;
    char lines[MAX_LINES][64], cur[64], text[96], all[MAX_LINES * 66] = "";
    int nlines = 0, shown = 0;
    size_t len = 0;

    for (const char *s = name; *s; s++) {
        seed = seed * 131 + (unsigned char)*s;
    }
    words = mib * 1024 * 1024 / sizeof *mem;
    mem = malloc(words * sizeof *mem);
    if (mem == NULL) {
        say("note %s: no memory for %zu MiB", name, mib);
        return 1;
    }
    for (size_t i = 0; i < words; i++) {
        mem[i] = word(i);
    }
    say("note %s: started (pid %d), %zu MiB filled", name, (int)getpid(), mib);
    screen_open(BASE_FONT);
    keys_open();

    for (;;) {
        int key = key_get();
        if (key == K_SHOWN) {
            size_t bad = check();
            shown++;
            all[0] = '\0';
            for (int i = 0; i < nlines; i++) {
                strcat(all, " [");
                strcat(all, lines[i]);
                strcat(all, "]");
            }
            if (bad == words) {
                say("note %s: shown %d, pid %d, memory intact (%zu MiB), lines:%s", name, shown,
                    (int)getpid(), mib, all);
            } else {
                say("note %s: shown %d, memory BAD at word %zu", name, shown, bad);
            }
            screen_fill(0, 0, screen_cols(), screen_rows(), BG);
            snprintf(text, sizeof text, "note %s", name);
            screen_bar(text, NULL);
            /* TRANSLATORS: Ctrl+Alt+Esc is a key name; keep it. */
            snprintf(text, sizeof text, _("note %s (pid %d): Ctrl+Alt+Esc for the menu"), name, (int)getpid());
            screen_text(1, 1, text, FG, BG);
            snprintf(text, sizeof text,
                     bad == words ? _("shown %d times; %zu MiB: intact") : _("shown %d times; %zu MiB: BAD"),
                     shown, mib);
            screen_text(1, 2, text, FG, BG);
            for (int i = 0; i < nlines; i++) {
                screen_text(1, 4 + i, lines[i], FG, BG);
            }
        } else if (key == '\n' && len == 4 && memcmp(cur, "call", 4) == 0) {
            /* run another note on top of this one and wait for it */
            char *callee[] = { "/usr/local/bin/note", "x", "16", NULL };
            len = 0;
            int status = base_call(callee);
            say("note %s: x returned %d", name, status);
            snprintf(text, sizeof text, _("x returned %d"), status);
            screen_fill(0, 3, screen_cols(), 1, BG);
            screen_text(1, 3, text, FG, BG);
        } else if (key == '\n' && len > 5 && memcmp(cur, "exit ", 5) == 0) {
            cur[len] = '\0';
            say("note %s: exiting with status %d", name, atoi(cur + 5));
            return atoi(cur + 5);
        } else if (key == '\n' && len > 0) {
            cur[len] = '\0';
            if (nlines == MAX_LINES) {
                memmove(lines, lines + 1, sizeof lines[0] * (MAX_LINES - 1));
                nlines--;
            }
            strcpy(lines[nlines], cur);
            screen_text(1, 4 + nlines, lines[nlines], FG, BG);
            nlines++;
            say("note %s: line [%s]", name, cur);
            len = 0;
        } else if (key == '\b' && len > 0) {
            do len--; /* a whole character */
            while (len > 0 && ((unsigned char)cur[len] & 0xC0) == 0x80);
        } else if (keys_typed(key) && len + strlen(keys_typed(key)) < sizeof cur) {
            memcpy(cur + len, keys_typed(key), strlen(keys_typed(key)));
            len += strlen(keys_typed(key));
        }
        snprintf(text, sizeof text, "> %.*s_ ", (int)len, cur);
        screen_fill(0, 5 + MAX_LINES, screen_cols(), 1, BG);
        screen_text(1, 5 + MAX_LINES, text, FG, BG);
        screen_flush();
    }
}
