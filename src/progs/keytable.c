/*
 * keytable KEYMAP OUT: the key table of a console keymap (rc.conf's keymap,
 * kbd's name, as the installer writes it), for keys.c. The X keyboard layout
 * of that keymap is the one systemd's kbd-model-map pairs with it (localectl's
 * table; a name it does not list is taken as an xkb layout), the one labwc is
 * given; libxkbcommon evaluates it. The direct programs, the lock, the
 * installer and process 1's passphrase prompt read the table, so a key types
 * the same in all of them and on the shared display.
 *
 * The first lines name the layout (console, layout, model, variant, options;
 * "-" for none) and the locale of the compose sequences. Then:
 *   altgr CODE       a key that is AltGr (ISO_Level3_Shift) in this layout
 *   key CODE NAME S0 .. S7
 *                    the key's keysym name, and for each state (1 Shift,
 *                    2 Caps Lock locked, 4 AltGr) its keysym and character,
 *                    "KEYSYM:CHAR" in hex, as xkb gives them; 0 for none
 *   compose KEYSYM.. : CHAR..
 *                    a dead key's (or Multi_key's) sequence and what it
 *                    types, from libxkbcommon's compose table for the
 *                    locale (LC_ALL, LC_CTYPE, LANG), with the keysyms this
 *                    layout has
 * Codes are evdev's. Written to OUT.tmp, then renamed. Exit 1 when the keymap
 * cannot be compiled; without a compose table, dead keys type nothing.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <xkbcommon/xkbcommon.h>
#include <xkbcommon/xkbcommon-compose.h>

#define KBD_MODEL_MAP "/usr/share/systemd/kbd-model-map"
#define NSTATES 8
#define MAX_SEQUENCE 6
#define MAX_RESULT 4

struct names { char layout[64], model[64], variant[64], options[256]; };

/* kbd-model-map's row for console: "console layout model variant options
 * [bcp47]", "-" for none. 0 when found. */
static int model_map(const char *path, const char *console, struct names *n)
{
    FILE *f = fopen(path, "r");
    char line[512];
    int found = -1;
    while (f && found < 0 && fgets(line, sizeof line, f)) {
        char c[64], l[64], m[64], v[64], o[256];
        if (line[0] == '#' || sscanf(line, "%63s %63s %63s %63s %255s", c, l, m, v, o) != 5) continue;
        if (strcmp(c, console) != 0) continue;
        snprintf(n->layout, sizeof n->layout, "%s", l);
        snprintf(n->model, sizeof n->model, "%s", strcmp(m, "-") ? m : "");
        snprintf(n->variant, sizeof n->variant, "%s", strcmp(v, "-") ? v : "");
        snprintf(n->options, sizeof n->options, "%s", strcmp(o, "-") ? o : "");
        found = 0;
    }
    if (f) fclose(f);
    return found;
}

/* The options without terminate:*: Ctrl+Alt+Backspace would end the shared
 * display, and the direct programs are not X. */
static void drop_terminate(char *options)
{
    char out[256] = "", *save = NULL;
    for (char *o = strtok_r(options, ",", &save); o; o = strtok_r(NULL, ",", &save)) {
        if (strncmp(o, "terminate:", 10) == 0) continue;
        size_t len = strlen(out);
        snprintf(out + len, sizeof out - len, "%s%s", len ? "," : "", o);
    }
    strcpy(options, out);
}

/* A key's character as keys.c gives it: Return as '\n', Shift+Tab as Tab
 * (programs see Shift held: backwards), no C1 controls; 0 for none. */
static unsigned text(xkb_keysym_t sym, uint32_t c)
{
    if (sym == XKB_KEY_ISO_Left_Tab) return '\t';
    if (c == '\r') return '\n';
    return c >= 0x7F && c < 0xA0 ? 0 : c;
}

/* The keysyms the layout gives, in any state: sorted, for bsearch. */
static xkb_keysym_t reach[256 * NSTATES];
static size_t nreach;

static int cmp_sym(const void *a, const void *b)
{
    xkb_keysym_t x = *(const xkb_keysym_t *)a, y = *(const xkb_keysym_t *)b;
    return x < y ? -1 : x > y;
}

static int reachable(xkb_keysym_t sym)
{
    return bsearch(&sym, reach, nreach, sizeof *reach, cmp_sym) != NULL;
}

/* The UTF-8 of a compose result as characters (at most MAX_RESULT); 0 for
 * none, or one that is not text. */
static int result_chars(const char *utf8, xkb_keysym_t sym, uint32_t *out)
{
    int n = 0;
    const unsigned char *p = (const unsigned char *)utf8;
    if (!p || !*p) {
        uint32_t c = xkb_keysym_to_utf32(sym);
        if (c < 0x20 || (c >= 0x7F && c < 0xA0)) return 0;
        out[0] = c;
        return 1;
    }
    while (*p) {
        uint32_t c = *p;
        int more = c < 0x80 ? 0 : (c & 0xE0) == 0xC0 ? 1 : (c & 0xF0) == 0xE0 ? 2 : (c & 0xF8) == 0xF0 ? 3 : -1;
        if (more < 0 || n == MAX_RESULT) return 0;
        c &= more ? 0x3Fu >> more : 0x7F;
        p++;
        for (int i = 0; i < more; i++, p++) {
            if ((*p & 0xC0) != 0x80) return 0;
            c = c << 6 | (*p & 0x3F);
        }
        if (c < 0x20 || (c >= 0x7F && c < 0xA0)) return 0;
        out[n++] = c;
    }
    return n;
}

static const char *compose_locale(void)
{
    static const char *const vars[] = { "LC_ALL", "LC_CTYPE", "LANG" };
    for (size_t i = 0; i < sizeof vars / sizeof vars[0]; i++) {
        const char *v = getenv(vars[i]);
        if (v && *v) return v;
    }
    return "C";
}

/* The sequences of the locale's compose table that start with a key that
 * types nothing (a dead key, Multi_key) and use only keysyms this layout
 * gives: the others could never be typed. The number written, -1 without a
 * table. */
static int write_compose(FILE *f, struct xkb_context *ctx, const char *locale)
{
    struct xkb_compose_table *table =
        xkb_compose_table_new_from_locale(ctx, locale, XKB_COMPOSE_COMPILE_NO_FLAGS);
    if (!table) return -1;
    struct xkb_compose_table_iterator *it = xkb_compose_table_iterator_new(table);
    int written = 0;
    for (struct xkb_compose_table_entry *e; it && (e = xkb_compose_table_iterator_next(it)) != NULL;) {
        size_t n;
        const xkb_keysym_t *seq = xkb_compose_table_entry_sequence(e, &n);
        uint32_t out[MAX_RESULT];
        if (n < 2 || n > MAX_SEQUENCE || xkb_keysym_to_utf32(seq[0]) != 0) continue;
        size_t k = 0;
        while (k < n && reachable(seq[k])) k++;
        if (k < n) continue;
        int nout = result_chars(xkb_compose_table_entry_utf8(e), xkb_compose_table_entry_keysym(e), out);
        if (nout == 0) continue;
        fprintf(f, "compose");
        for (k = 0; k < n; k++) fprintf(f, " %x", seq[k]);
        fprintf(f, " :");
        for (int i = 0; i < nout; i++) fprintf(f, " %x", out[i]);
        fputc('\n', f);
        written++;
    }
    xkb_compose_table_iterator_free(it);
    xkb_compose_table_unref(table);
    return written;
}

int main(int argc, char **argv)
{
    if (argc != 3) {
        fprintf(stderr, "usage: keytable KEYMAP OUT\n");
        return 2;
    }
    const char *console = argv[1], *out = argv[2];
    const char *map = getenv("LOCACHY_KBD_MODEL_MAP"); /* tests: their own copy */
    struct names n = { "", "", "", "" };
    if (model_map(map ? map : KBD_MODEL_MAP, console, &n) < 0) {
        snprintf(n.layout, sizeof n.layout, "%s", console);
        snprintf(n.model, sizeof n.model, "pc105");
    }
    drop_terminate(n.options);
    struct xkb_context *ctx = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
    struct xkb_rule_names rmlvo = { .rules = "evdev", .model = n.model, .layout = n.layout,
                                    .variant = n.variant, .options = n.options };
    struct xkb_keymap *km = ctx ? xkb_keymap_new_from_names(ctx, &rmlvo, XKB_KEYMAP_COMPILE_NO_FLAGS) : NULL;
    if (!km) {
        fprintf(stderr, "keytable: no X keyboard layout for keymap %s (layout %s)\n", console, n.layout);
        return 1;
    }
    struct xkb_state *st = xkb_state_new(km);
    if (!st) {
        perror("keytable");
        return 1;
    }
    /* The modifiers of the states: Shift and Lock by name, AltGr as its
     * key sets it (Mod5 in xkeyboard-config, whatever a layout makes it). */
    xkb_mod_mask_t shift = 1u << xkb_keymap_mod_get_index(km, XKB_MOD_NAME_SHIFT);
    xkb_mod_mask_t lock = 1u << xkb_keymap_mod_get_index(km, XKB_MOD_NAME_CAPS), altgr = 0;
    int altgr_code[256], naltgr = 0;
    for (int code = 1; code < 256; code++) {
        if (xkb_state_key_get_one_sym(st, (xkb_keycode_t)code + 8) != XKB_KEY_ISO_Level3_Shift) continue;
        altgr_code[naltgr++] = code;
        if (!altgr) {
            struct xkb_state *press = xkb_state_new(km);
            xkb_state_update_key(press, (xkb_keycode_t)code + 8, XKB_KEY_DOWN);
            altgr = xkb_state_serialize_mods(press, XKB_STATE_MODS_DEPRESSED);
            xkb_state_unref(press);
        }
    }
    static xkb_keysym_t syms[256][NSTATES];
    static uint32_t chars[256][NSTATES];
    for (int code = 1; code < 256; code++) {
        for (int s = 0; s < NSTATES; s++) {
            xkb_state_update_mask(st, (s & 1 ? shift : 0) | (s & 4 ? altgr : 0), 0, s & 2 ? lock : 0, 0, 0, 0);
            xkb_keycode_t kc = (xkb_keycode_t)code + 8;
            syms[code][s] = xkb_state_key_get_one_sym(st, kc);
            chars[code][s] = text(syms[code][s], xkb_state_key_get_utf32(st, kc));
            if (syms[code][s] != XKB_KEY_NoSymbol) reach[nreach++] = syms[code][s];
        }
    }
    qsort(reach, nreach, sizeof *reach, cmp_sym);
    char tmp[4096];
    snprintf(tmp, sizeof tmp, "%s.tmp", out);
    FILE *f = fopen(tmp, "w");
    if (!f) {
        perror("keytable");
        return 1;
    }
    const char *locale = compose_locale();
    fprintf(f, "# LOCachy key table (keytable) for keymap %s\nconsole %s\nlayout %s\nmodel %s\nvariant %s\n"
            "options %s\nlocale %s\n", console, console, n.layout[0] ? n.layout : "-", n.model[0] ? n.model : "-",
            n.variant[0] ? n.variant : "-", n.options[0] ? n.options : "-", locale);
    for (int i = 0; i < naltgr; i++) fprintf(f, "altgr %d\n", altgr_code[i]);
    for (int code = 1; code < 256; code++) {
        char name[64];
        if (syms[code][0] == XKB_KEY_NoSymbol || xkb_keysym_get_name(syms[code][0], name, sizeof name) <= 0) continue;
        fprintf(f, "key %d %s", code, name);
        for (int s = 0; s < NSTATES; s++) fprintf(f, " %x:%x", syms[code][s], chars[code][s]);
        fputc('\n', f);
    }
    if (write_compose(f, ctx, locale) < 0) {
        fprintf(stderr, "keytable: no compose table for locale %s: dead keys type nothing\n", locale);
    }
    if (fclose(f) != 0 || rename(tmp, out) != 0) {
        perror("keytable");
        remove(tmp);
        return 1;
    }
    xkb_state_unref(st);
    xkb_keymap_unref(km);
    xkb_context_unref(ctx);
    return 0;
}
