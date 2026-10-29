/*
 * The keyboard through evdev, plus configurable global keybindings from
 * /etc/locachy/rc.conf (key_home, key_lock, key_ime, key_ime_cn, key_ime_raw,
 * key_ime_width, key_volume_up, key_volume_down, key_volume_mute,
 * key_mic_mute). Keys type as the layout's key table has them (keytable):
 * with Shift, Caps Lock and AltGr, any character (K_TEXT beyond ASCII), and
 * the dead keys' sequences of libxkbcommon's compose table. A keyboard named
 * in keyboards.conf types with its own keymap's table, the others with the
 * system's (rc.conf's keymap); every keyboard's modifiers count together,
 * each by its own table's roles, and Caps Lock is one.
 */
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/input.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include "internal.h"

#define MAX_KEYBOARDS 8
#define MAX_BINDINGS  32

/* US, when there is no key table. */
static const char normal[] = "\0\0331234567890-=\b\tqwertyuiop[]\n\0asdfghjkl;'`\0\\zxcvbnm,./\0*\0 ";
static const char shifted[] = "\0\033!@#$%^&*()_+\b\tQWERTYUIOP{}\n\0ASDFGHJKL:\"~\0|ZXCVBNM<>?\0*\0 ";

/* The layout's key table, written by keytable from rc.conf's keymap: for
 * each key and state (1 Shift, 2 Caps Lock, 4 AltGr) its keysym and the
 * character it types (Unicode, 0 for none), its keysym name (Henkan_Mode),
 * the keys that are AltGr, the dead keys' sequences, and the layout's X
 * names (the shared display's: locachy-seat). Read at the first key, again
 * after keys_reload. */
#ifndef KEYS_TABLE
#define KEYS_TABLE "/usr/local/share/locachy/keymap.table"
#endif
/* Keyboards with a keymap of their own: "IDENTIFIER KEYMAP" lines (the
 * identifier as keys_device_id gives it, the keymap a console keymap name as
 * rc.conf's), and their keymaps' tables, KEYS_TABLES/KEYMAP.table (make.sh
 * and settings write them with keytable). */
#ifndef KEYS_DEVICES
#define KEYS_DEVICES "/etc/locachy/keyboards.conf"
#endif
#ifndef KEYS_TABLES
#define KEYS_TABLES "/usr/local/share/locachy/keymaps"
#endif
#define NSTATES 8
#define MAX_SEQUENCE 6
#define MAX_RESULT 4
struct sequence {
    uint32_t keys[MAX_SEQUENCE], out[MAX_RESULT];
    unsigned char n, nout;
};

struct binding {
    int code;
    uint8_t ctrl, alt, shift, meta;
    int action;
};

/* A key table, and rc.conf's keys bound to its keys. */
struct table {
    uint32_t syms[NSTATES][256], chars[NSTATES][256];
    char keysym[256][24];
    unsigned char altgr_key[256], modifier_role[256];
    struct sequence *sequences;
    int nsequences;
    char layout_names[5][64]; /* console, layout, model, variant, options */
    char keymap[64];          /* keyboards.conf's name of it; the system's: "" */
    struct binding bindings[MAX_BINDINGS];
    int nbindings, bindings_loaded;
};
/* The system's first, then those keyboards use. */
#define MAX_TABLES (MAX_KEYBOARDS + 1)
static struct table tables[MAX_TABLES];
static int ntables = 1;
static uint32_t pending[MAX_SEQUENCE]; /* the dead keys' sequence being typed */
static int npending;
static char typed_text[4 * MAX_RESULT + 1]; /* K_TEXT's */
static const char *const layout_fields[5] = { "console", "layout", "model", "variant", "options" };
static int table_read;

static int keyboards[MAX_KEYBOARDS];
static char paths[MAX_KEYBOARDS][32]; /* process 1: to open them again */
static signed char device_table[MAX_KEYBOARDS]; /* each one's table; -1: not looked up yet */
static int count;
/* Track physical keys independently even when two share a modifier role. */
static unsigned char modifiers[MAX_KEYBOARDS][256], unowned_modifiers[256];
static int shift, ctrl, alt, meta, altgr;
static int caps; /* Caps Lock locked: letters in capitals (with Shift, small) */
static int caps_code; /* a key that was Caps Lock only with Shift when pressed (jp's Eisu) */
static int last_code; /* the last key pressed, not a modifier */
static struct table *last_table = &tables[0]; /* that key's keyboard's */
static int name_any;  /* keys_name_any */

/* The input method (ime.c), in the programs that have it: Caps Lock's key
 * and state (Rime passes keys by while it is locked). */
__attribute__((weak)) void ime_caps_key(int pressed, int locked);
__attribute__((weak)) void ime_set_caps(int locked);


static int has(const unsigned long *bits, int n)
{
    return (bits[n / (8 * sizeof(long))] >> (n % (8 * sizeof(long)))) & 1;
}

static void add_binding(struct table *tb, int code, int c, int a, int s, int m, int action)
{
    if (tb->nbindings >= MAX_BINDINGS) {
        return;
    }
    tb->bindings[tb->nbindings++] = (struct binding){
        .code = code,
        .ctrl = (uint8_t)c,
        .alt = (uint8_t)a,
        .shift = (uint8_t)s,
        .meta = (uint8_t)m,
        .action = action
    };
}

/* US, without a table: Caps Lock turns letters only, as xkb's Lock does;
 * no AltGr, no dead keys. */
static void us_states(struct table *tb)
{
    tb->modifier_role[KEY_LEFTSHIFT] = tb->modifier_role[KEY_RIGHTSHIFT] = KEYS_SHIFT;
    tb->modifier_role[KEY_LEFTCTRL] = tb->modifier_role[KEY_RIGHTCTRL] = KEYS_CTRL;
    tb->modifier_role[KEY_LEFTALT] = tb->modifier_role[KEY_RIGHTALT] = KEYS_ALT;
    tb->modifier_role[KEY_LEFTMETA] = tb->modifier_role[KEY_RIGHTMETA] = KEYS_META;
    tb->modifier_role[KEY_CAPSLOCK] = KEYS_CAPS;
    for (size_t i = 0; i < sizeof normal - 1; i++) {
        uint32_t n = (unsigned char)normal[i], u = (unsigned char)shifted[i];
        int letter = isalpha((int)n);
        for (int st = 0; st < NSTATES; st++) {
            int up = !!(st & 1) != (letter && (st & 2));
            tb->chars[st][i] = up ? u : n;
        }
    }
}

static int modifier_keysym(uint32_t sym)
{
    switch (sym) {
    case 0xffe1: case 0xffe2: return KEYS_SHIFT;
    case 0xffe3: case 0xffe4: return KEYS_CTRL;
    case 0xffe7: case 0xffe8: case 0xffe9: case 0xffea: return KEYS_ALT;
    case 0xffeb: case 0xffec: case 0xffed: case 0xffee: return KEYS_META;
    case 0xfe03: case 0xfe11: case 0xff7e: return KEYS_ALTGR;
    case 0xffe5: case 0xffe6: return KEYS_CAPS;
    default: return 0;
    }
}

/* The table at path into tb: 0, or -1 when there is none (tb types US). */
static int load_table(struct table *tb, const char *path)
{
    free(tb->sequences);
    memset(tb, 0, sizeof *tb);
    us_states(tb);
    FILE *f = path ? fopen(path, "re") : NULL;
    if (!f) return -1;
    static uint32_t got_syms[NSTATES][256], got_chars[NSTATES][256];
    memset(got_syms, 0, sizeof got_syms);
    memset(got_chars, 0, sizeof got_chars);
    char names[256][24] = { { 0 } }, line[512];
    unsigned char roles[256] = { 0 };
    int keys = 0, size = 0, explicit_roles = 0;
    while (fgets(line, sizeof line, f)) {
        int code, at;
        char name[24], field[16], value[64];
        if (sscanf(line, "key %d %23s%n", &code, name, &at) == 2) {
            if (code < 1 || code > 255) continue;
            for (int st = 0; st < NSTATES; st++) {
                unsigned sym, ch;
                int used;
                if (sscanf(line + at, " %x:%x%n", &sym, &ch, &used) != 2) break;
                got_syms[st][code] = sym;
                got_chars[st][code] = ch < 0x110000 ? ch : 0;
                at += used;
            }
            memcpy(names[code], name, sizeof name);
            keys++;
        } else if (sscanf(line, "modifier %d %d", &code, &at) == 2) {
            if (code > 0 && code < 256 && at >= 0 && at < 64) {
                roles[code] = (unsigned char)at;
                explicit_roles = 1;
            }
        } else if (sscanf(line, "altgr %d", &code) == 1) {
            if (code > 0 && code < 256) tb->altgr_key[code] = 1;
        } else if (strncmp(line, "compose ", 8) == 0) {
            struct sequence q = { .n = 0 };
            char *t = line + 8, *end;
            while (q.n < MAX_SEQUENCE && *t && *t != ':') {
                unsigned long v = strtoul(t, &end, 16);
                if (end == t) break;
                q.keys[q.n++] = (uint32_t)v;
                t = end + strspn(end, " ");
            }
            if (*t++ != ':' || q.n < 2) continue;
            for (unsigned long v; q.nout < MAX_RESULT && (v = strtoul(t, &end, 16), end != t); t = end) {
                q.out[q.nout++] = v < 0x110000 ? (uint32_t)v : 0xFFFD;
            }
            if (q.nout == 0) continue;
            if (tb->nsequences == size) {
                size = size ? 2 * size : 256;
                struct sequence *more = realloc(tb->sequences, (size_t)size * sizeof *tb->sequences);
                if (!more) continue;
                tb->sequences = more;
            }
            tb->sequences[tb->nsequences++] = q;
        } else if (sscanf(line, "%15s %63s", field, value) == 2) {
            for (int i = 0; i < 5; i++) {
                if (strcmp(field, layout_fields[i]) == 0 && strcmp(value, "-") != 0) {
                    snprintf(tb->layout_names[i], sizeof tb->layout_names[i], "%s", value);
                }
            }
        }
    }
    fclose(f);
    if (keys == 0) { /* US, as without a table */
        memset(tb->altgr_key, 0, sizeof tb->altgr_key);
        free(tb->sequences);
        tb->sequences = NULL;
        tb->nsequences = 0;
        return -1;
    }
    memcpy(tb->syms, got_syms, sizeof tb->syms);
    memcpy(tb->chars, got_chars, sizeof tb->chars);
    memcpy(tb->keysym, names, sizeof tb->keysym);
    memset(tb->modifier_role, 0, sizeof tb->modifier_role);
    for (int code = 1; code < 256; code++) {
        tb->modifier_role[code] = explicit_roles ? roles[code] :
                                  (unsigned char)(tb->altgr_key[code] ? KEYS_ALTGR : modifier_keysym(tb->syms[0][code]));
    }
    return 0;
}

/* The keyboards' own tables go; each keyboard's is looked up again. */
static void drop_device_tables(void)
{
    for (int t = 1; t < ntables; t++) {
        free(tables[t].sequences);
        tables[t].sequences = NULL;
    }
    ntables = 1;
    memset(device_table, -1, sizeof device_table);
    last_table = &tables[0];
}

int keys_layout_load(const char *path)
{
    table_read = 1;
    npending = 0;
    drop_device_tables();
    return load_table(&tables[0], path);
}

static void read_table(void)
{
    if (!table_read) keys_layout_load(KEYS_TABLE);
}

int keys_device_id(int fd, char *out, size_t size)
{
    struct input_id id;
    char name[256] = "";
    if (ioctl(fd, EVIOCGID, &id) < 0 || ioctl(fd, EVIOCGNAME(sizeof name - 1), name) < 0) {
        return -1;
    }
    /* As sway names input devices: the name without white space around it,
     * its spaces and unprintable bytes as _. */
    char *s = name, *e = name + strlen(name);
    while (isspace((unsigned char)*s)) s++;
    while (e > s && isspace((unsigned char)e[-1])) *--e = '\0';
    for (char *p = s; *p; p++) {
        if (*p == ' ' || !isprint((unsigned char)*p)) *p = '_';
    }
    snprintf(out, size, "%u:%u:%s", id.vendor, id.product, s);
    return 0;
}

/* A console keymap's name as keyboards.conf and the tables' file names have
 * it: letters, digits, . _ -, not first a dot. */
static int keymap_name(const char *s)
{
    if (!s[0] || s[0] == '.' || strlen(s) >= sizeof tables[0].keymap) return 0;
    for (; *s; s++) {
        if (!isalnum((unsigned char)*s) && !strchr("._-", *s)) return 0;
    }
    return 1;
}

/* keyboards.conf's keymap for the keyboard id: 0, or -1 when it has none. */
static int device_keymap(const char *id, char *keymap, size_t size)
{
    FILE *f = fopen(KEYS_DEVICES, "re");
    char line[512], name[KEYS_ID_SIZE], map[64];
    int found = -1;
    while (f && found < 0 && fgets(line, sizeof line, f)) {
        if (line[0] == '#' || sscanf(line, "%299s %63s", name, map) != 2) continue;
        if (strcmp(name, id) == 0 && keymap_name(map)) {
            snprintf(keymap, size, "%s", map);
            found = 0;
        }
    }
    if (f) fclose(f);
    return found;
}

/* Keyboard fd's table: its own keymap's (keyboards.conf), read once for all
 * keyboards with that keymap; the system's for the others, and when its own
 * table is missing. */
static int resolve(int fd)
{
    char id[KEYS_ID_SIZE], keymap[64], path[sizeof KEYS_TABLES + 72];
    if (keys_device_id(fd, id, sizeof id) < 0 || device_keymap(id, keymap, sizeof keymap) < 0 ||
        strcmp(keymap, tables[0].layout_names[0]) == 0) {
        return 0;
    }
    for (int t = 1; t < ntables; t++) {
        if (strcmp(tables[t].keymap, keymap) == 0) return t;
    }
    snprintf(path, sizeof path, "%s/%s.table", KEYS_TABLES, keymap);
    if (ntables == MAX_TABLES || load_table(&tables[ntables], path) < 0) {
        say("keys: no key table %s for keyboard %s; it types as the system's", path, id);
        return 0;
    }
    snprintf(tables[ntables].keymap, sizeof tables[0].keymap, "%s", keymap);
    return ntables++;
}

static struct table *device_table_of(int i)
{
    read_table();
    if (device_table[i] < 0) device_table[i] = (signed char)resolve(keyboards[i]);
    return &tables[device_table[i]];
}

/* The table fd types with (one not among the keyboards: the system's). */
static struct table *table_of(int fd)
{
    for (int i = 0; i < count; i++) {
        if (keyboards[i] == fd) return device_table_of(i);
    }
    read_table();
    return &tables[0];
}

int keys_modifier(int code)
{
    read_table();
    return code > 0 && code < 256 ? tables[0].modifier_role[code] : 0;
}

static const char *layout_name(const struct table *tb, const char *field)
{
    for (int i = 0; i < 5; i++) {
        if (strcmp(field, layout_fields[i]) == 0) return tb->layout_names[i][0] ? tb->layout_names[i] : NULL;
    }
    return NULL;
}

const char *keys_layout_name(const char *field)
{
    read_table();
    return layout_name(&tables[0], field);
}

const char *keys_device_layout(int fd, const char *field)
{
    return layout_name(table_of(fd), field);
}

static int table_char(const struct table *tb, int code, int with_shift)
{
    uint32_t c = code > 0 && code < 256 ? tb->chars[!!with_shift][code] : 0;
    return c < 0x80 ? (int)c : 0;
}

int keys_char(int code, int with_shift)
{
    read_table();
    return table_char(&tables[0], code, with_shift);
}

int keys_device_char(int fd, int code, int with_shift)
{
    return table_char(table_of(fd), code, with_shift);
}

const char *keys_text(void)
{
    return typed_text;
}

/* Keys that never stand alone in a binding: the modifiers, the locks, Esc. */
static int modifier_name(const char *name)
{
    static const char *const names[] = { "Shift_L", "Shift_R", "Control_L", "Control_R", "Alt_L", "Alt_R",
        "Meta_L", "Meta_R", "Super_L", "Super_R", "Hyper_L", "Hyper_R", "ISO_Level3_Shift",
        "ISO_Level5_Shift", "Mode_switch", "Caps_Lock", "Num_Lock", "Scroll_Lock", "Escape" };
    for (size_t i = 0; i < sizeof names / sizeof names[0]; i++) {
        if (strcmp(name, names[i]) == 0) return 1;
    }
    return 0;
}

static int parse_key_name(const struct table *tb, const char *name)
{
    if (strcasecmp(name, "Esc") == 0 || strcasecmp(name, "Escape") == 0) return KEY_ESC;
    if (strcasecmp(name, "Space") == 0) return KEY_SPACE;
    if (strcasecmp(name, "Enter") == 0 || strcasecmp(name, "Return") == 0) return KEY_ENTER;
    if (strcasecmp(name, "Tab") == 0) return KEY_TAB;
    if (strcasecmp(name, "BackSpace") == 0) return KEY_BACKSPACE;
    if (strcasecmp(name, "ThinkVantage") == 0 || strcasecmp(name, "Vendor") == 0) return KEY_VENDOR;
    if (strcasecmp(name, "Prog1") == 0) return KEY_PROG1;
    if (strcasecmp(name, "VolumeUp") == 0) return KEY_VOLUMEUP;
    if (strcasecmp(name, "VolumeDown") == 0) return KEY_VOLUMEDOWN;
    if (strcasecmp(name, "Mute") == 0 || strcasecmp(name, "VolumeMute") == 0) return KEY_MUTE;
    if (strcasecmp(name, "MicMute") == 0) return KEY_MICMUTE;
    if (strcasecmp(name, "BrightnessUp") == 0) return KEY_BRIGHTNESSUP;
    if (strcasecmp(name, "BrightnessDown") == 0) return KEY_BRIGHTNESSDOWN;
    if ((name[0] == 'F' || name[0] == 'f') && isdigit((unsigned char)name[1])) {
        int fn = atoi(name + 1);
        if (fn >= 1 && fn <= 10) return KEY_F1 + fn - 1;
        if (fn == 11) return KEY_F11;
        if (fn == 12) return KEY_F12;
    }
    if (name[0] != '\0' && name[1] == '\0') {
        uint32_t ch = (uint32_t)tolower((unsigned char)name[0]);
        for (int i = 1; i < 256; i++) {
            if (tb->chars[0][i] == ch) {
                return i;
            }
        }
    }
    /* The layout's keys without a character, by keysym name (Henkan_Mode). */
    for (int i = 1; i < 256; i++) {
        if (tb->keysym[i][0] && !tb->chars[0][i] && strcasecmp(tb->keysym[i], name) == 0) {
            return i;
        }
    }
    return -1;
}

static void parse_binding_spec(struct table *tb, const char *spec, int action)
{
    char buf[256];
    snprintf(buf, sizeof buf, "%s", spec);
    for (char *p = buf; *p; p++) {
        if (*p == ',') *p = ' ';
    }
    char *save = NULL;
    for (char *tok = strtok_r(buf, " \t", &save); tok != NULL;
         tok = strtok_r(NULL, " \t", &save)) {
        int c = 0, a = 0, s = 0, m = 0, code = -1;
        char *part = tok;
        while (part && *part) {
            char *plus = strchr(part, '+');
            if (plus) *plus = '\0';
            if (strcasecmp(part, "Ctrl") == 0 || strcasecmp(part, "Control") == 0) {
                c = 1;
            } else if (strcasecmp(part, "Alt") == 0) {
                a = 1;
            } else if (strcasecmp(part, "Shift") == 0) {
                s = 1;
            } else if (strcasecmp(part, "Super") == 0 || strcasecmp(part, "Meta") == 0 ||
                       strcasecmp(part, "Win") == 0) {
                m = 1;
            } else {
                code = parse_key_name(tb, part);
            }
            part = plus ? plus + 1 : NULL;
        }
        if (code == -1 && (c || a || s || m)) {
            add_binding(tb, 0, c, a, s, m, action);
        } else if (code >= 0) {
            add_binding(tb, code, c, a, s, m, action);
            if (code == KEY_VENDOR) {
                add_binding(tb, KEY_PROG1, c, a, s, m, action);
            }
        }
    }
}

struct key_cfg {
    char home[256];
    char lock[128];
    char ime[128];
    char ime_cn[128];
    char ime_raw[128];
    char ime_width[128];
    char vol_up[128];
    char vol_down[128];
    char vol_mute[128];
    char mic_mute[128];
    char bri_up[128];
    char bri_down[128];
};

static void on_key_rc(const char *name, const char *value, void *arg)
{
    struct key_cfg *cfg = arg;
    if (strcmp(name, "key_home") == 0) snprintf(cfg->home, sizeof cfg->home, "%s", value);
    else if (strcmp(name, "key_lock") == 0) snprintf(cfg->lock, sizeof cfg->lock, "%s", value);
    else if (strcmp(name, "key_ime") == 0) snprintf(cfg->ime, sizeof cfg->ime, "%s", value);
    else if (strcmp(name, "key_ime_cn") == 0) snprintf(cfg->ime_cn, sizeof cfg->ime_cn, "%s", value);
    else if (strcmp(name, "key_ime_raw") == 0) snprintf(cfg->ime_raw, sizeof cfg->ime_raw, "%s", value);
    else if (strcmp(name, "key_ime_width") == 0) snprintf(cfg->ime_width, sizeof cfg->ime_width, "%s", value);
    else if (strcmp(name, "key_volume_up") == 0) snprintf(cfg->vol_up, sizeof cfg->vol_up, "%s", value);
    else if (strcmp(name, "key_volume_down") == 0) snprintf(cfg->vol_down, sizeof cfg->vol_down, "%s", value);
    else if (strcmp(name, "key_volume_mute") == 0) snprintf(cfg->vol_mute, sizeof cfg->vol_mute, "%s", value);
    else if (strcmp(name, "key_mic_mute") == 0) snprintf(cfg->mic_mute, sizeof cfg->mic_mute, "%s", value);
    else if (strcmp(name, "key_brightness_up") == 0) snprintf(cfg->bri_up, sizeof cfg->bri_up, "%s", value);
    else if (strcmp(name, "key_brightness_down") == 0) snprintf(cfg->bri_down, sizeof cfg->bri_down, "%s", value);
}

/* rc.conf's keys, read once (again after keys_reload), bound to the keys of
 * each table that names them. */
static struct key_cfg key_cfg;
static int key_cfg_read;

static void load_keybindings(struct table *tb)
{
    read_table();
    if (tb->bindings_loaded) {
        return;
    }
    tb->bindings_loaded = 1;
    if (!key_cfg_read) {
        /* The input method's keys on a JIS keyboard, by keysym name (keyboards
         * without these keys bind nothing): Henkan, right of the space bar,
         * Chinese; Muhenkan, left of it, direct input; Zenkaku_Hankaku, left of
         * 1 in the jp layout, the width of Chinese mode's punctuation. */
        key_cfg = (struct key_cfg){
            .home = "Super+Esc Ctrl+Alt+Esc Ctrl+Alt+Super ThinkVantage",
            .lock = "Super+L",
            .ime = "Ctrl+Space",
            .ime_cn = "Henkan_Mode",
            .ime_raw = "Muhenkan",
            .ime_width = "Zenkaku_Hankaku",
            .vol_up = "VolumeUp",
            .vol_down = "VolumeDown",
            .vol_mute = "Mute",
            .mic_mute = "MicMute",
            .bri_up = "BrightnessUp",
            .bri_down = "BrightnessDown",
        };
        rc_read(on_key_rc, &key_cfg);
        key_cfg_read = 1;
    }
    tb->nbindings = 0;
    parse_binding_spec(tb, key_cfg.home, K_ACT_HOME);
    parse_binding_spec(tb, key_cfg.lock, K_ACT_LOCK);
    parse_binding_spec(tb, key_cfg.ime, K_CTRL_SPACE);
    parse_binding_spec(tb, key_cfg.ime_cn, K_ACT_IME_CN);
    parse_binding_spec(tb, key_cfg.ime_raw, K_ACT_IME_RAW);
    parse_binding_spec(tb, key_cfg.ime_width, K_ACT_IME_WIDTH);
    parse_binding_spec(tb, key_cfg.vol_up, K_VOL_UP);
    parse_binding_spec(tb, key_cfg.vol_down, K_VOL_DOWN);
    parse_binding_spec(tb, key_cfg.vol_mute, K_VOL_MUTE);
    parse_binding_spec(tb, key_cfg.mic_mute, K_MIC_MUTE);
    parse_binding_spec(tb, key_cfg.bri_up, K_BRI_UP);
    parse_binding_spec(tb, key_cfg.bri_down, K_BRI_DOWN);
}

void keys_reload(void)
{
    key_cfg_read = 0;
    tables[0].bindings_loaded = 0;
    table_read = 0;
    npending = 0;
    drop_device_tables();
}

static int match_action(struct table *tb, int ev_code, int c, int a, int s, int m)
{
    load_keybindings(tb);
    int is_mod_key = ev_code > 0 && ev_code < 256 &&
                     (tb->modifier_role[ev_code] & (KEYS_CTRL | KEYS_ALT | KEYS_SHIFT | KEYS_META));
    for (int i = 0; i < tb->nbindings; i++) {
        const struct binding *b = &tb->bindings[i];
        if (b->code == 0) {
            if (is_mod_key && b->ctrl == !!c && b->alt == !!a &&
                b->shift == !!s && b->meta == !!m) {
                return b->action;
            }
        } else if (b->code == ev_code) {
            if (!b->ctrl && !b->alt && !b->shift && !b->meta && ev_code >= KEY_MUTE) {
                return b->action;
            }
            if (b->ctrl == !!c && b->alt == !!a && b->shift == !!s && b->meta == !!m) {
                return b->action;
            }
        }
    }
    return 0;
}

int keys_match_action(int ev_code, int c, int a, int s, int m)
{
    read_table();
    return match_action(&tables[0], ev_code, c, a, s, m);
}

int keys_device_action(int fd, int ev_code, int c, int a, int s, int m)
{
    return match_action(table_of(fd), ev_code, c, a, s, m);
}

int keys_alone_action(int code)
{
    return code >= KEY_MUTE ? keys_match_action(code, 0, 0, 0, 0) : 0;
}

int keys_open(void)
{
    load_keybindings(&tables[0]);
    if (control_fd() >= 0) {
        control_wait_take(); /* keys_take does the rest */
        pointer_open();      /* and the pointer comes after each take */
        return count;
    }
    keys_refresh();
    return count;
}

/* Layout roles, held independently by every key and every device, each by
 * its keyboard's table. */
static void update_modifiers(void)
{
    int held = 0;
    for (int code = 1; code < 256; code++) {
        if (unowned_modifiers[code]) held |= tables[0].modifier_role[code];
        for (int i = 0; i < count; i++) {
            if (modifiers[i][code]) held |= device_table_of(i)->modifier_role[code];
        }
    }
    shift = !!(held & KEYS_SHIFT);
    ctrl = !!(held & KEYS_CTRL);
    alt = !!(held & KEYS_ALT);
    meta = !!(held & KEYS_META);
    altgr = !!(held & KEYS_ALTGR);
}

/* Preserve live queues: reopening every device on an unrelated uevent loses
 * unread keys, and a path alone cannot distinguish a reconnected device. */
void keys_refresh(void)
{
    load_keybindings(&tables[0]);
    for (int i = 0; i < count;) {
        int version;
        if (ioctl(keyboards[i], EVIOCGVERSION, &version) >= 0) { i++; continue; }
        close(keyboards[i]);
        count--;
        memmove(keyboards + i, keyboards + i + 1, (size_t)(count - i) * sizeof *keyboards);
        memmove(paths + i, paths + i + 1, (size_t)(count - i) * sizeof *paths);
        memmove(modifiers + i, modifiers + i + 1, (size_t)(count - i) * sizeof *modifiers);
        memmove(device_table + i, device_table + i + 1, (size_t)(count - i) * sizeof *device_table);
    }
    update_modifiers();
    for (int i = 0; i < 64 && count < MAX_KEYBOARDS; i++) {
        unsigned long bits[KEY_MAX / (8 * sizeof(long)) + 1] = { 0 };
        char path[sizeof paths[0]];
        snprintf(path, sizeof path, "/dev/input/event%d", i);
        int fd = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
        if (fd < 0) {
            continue;
        }
        int known = 0;
        for (int k = 0; k < count; k++) known |= input_same(keyboards[k], fd);
        if (known) { close(fd); continue; }
        int wanted = 0;
        if (ioctl(fd, EVIOCGBIT(EV_KEY, sizeof bits), bits) >= 0) {
            if (has(bits, KEY_A) && has(bits, KEY_ENTER)) {
                wanted = 1;
            }
            for (int b = 0; !wanted && b < tables[0].nbindings; b++) {
                if (tables[0].bindings[b].code >= KEY_MUTE && has(bits, tables[0].bindings[b].code)) {
                    wanted = 1;
                }
            }
        }
        if (wanted) {
            snprintf(paths[count], sizeof paths[count], "%s", path);
            memset(modifiers[count], 0, sizeof modifiers[count]);
            device_table[count] = -1;
            keyboards[count++] = fd;
        } else {
            close(fd);
        }
    }
}

void keys_take(const int *fds, int n)
{
    keys_release();
    for (int i = 0; i < n; i++) {
        if (count < MAX_KEYBOARDS) {
            device_table[count] = -1;
            keyboards[count++] = fds[i];
        } else {
            close(fds[i]);
        }
    }
}

/* An input-only update is not a new screen activation. Keep live queues and
 * modifier state; discard only descriptors for devices no longer lent. */
void keys_update(const int *fds, int n)
{
    int old[MAX_KEYBOARDS], used[MAX_KEYBOARDS] = { 0 }, old_count = count;
    unsigned char old_modifiers[MAX_KEYBOARDS][256];
    signed char old_tables[MAX_KEYBOARDS];
    memcpy(old, keyboards, (size_t)old_count * sizeof *old);
    memcpy(old_modifiers, modifiers, (size_t)old_count * sizeof *modifiers);
    memcpy(old_tables, device_table, (size_t)old_count * sizeof *device_table);
    count = 0;
    for (int i = 0; i < n; i++) {
        if (count == MAX_KEYBOARDS) { close(fds[i]); continue; }
        int k;
        for (k = 0; k < old_count; k++) {
            if (!used[k] && input_same(old[k], fds[i])) break;
        }
        if (k < old_count) {
            keyboards[count] = old[k];
            device_table[count] = old_tables[k];
            memcpy(modifiers[count++], old_modifiers[k], sizeof modifiers[0]);
            used[k] = 1;
            close(fds[i]);
        } else {
            keyboards[count] = fds[i];
            device_table[count] = -1;
            memset(modifiers[count++], 0, sizeof modifiers[0]);
        }
    }
    for (int i = 0; i < old_count; i++) if (!used[i]) close(old[i]);
    update_modifiers();
}

void keys_release(void)
{
    for (int i = 0; i < count; i++) {
        close(keyboards[i]);
    }
    count = 0;
    memset(modifiers, 0, sizeof modifiers);
    memset(unowned_modifiers, 0, sizeof unowned_modifiers);
    shift = ctrl = alt = meta = altgr = 0;
    npending = 0;
}

int keys_sync(int *fds, int *n, int max)
{
    int changed = 0;
    for (int i = 0; i < *n;) {
        int found = 0;
        for (int k = 0; k < count; k++) found |= input_same(fds[i], keyboards[k]);
        if (found) { i++; continue; }
        keys_take_back(fds + i, 1);
        --*n;
        memmove(fds + i, fds + i + 1, (size_t)(*n - i) * sizeof *fds);
        changed = 1;
    }
    for (int i = 0; i < count && *n < max; i++) {
        int found = 0;
        for (int k = 0; k < *n; k++) found |= input_same(fds[k], keyboards[i]);
        if (found) continue;
        int fd = open(paths[i], O_RDONLY | O_NONBLOCK | O_CLOEXEC);
        if (fd < 0) continue;
        if (!input_same(fd, keyboards[i])) { close(fd); continue; }
        fds[(*n)++] = fd;
        changed = 1;
    }
    return changed;
}

int keys_lend(int *fds, int max)
{
    int n = 0;
    keys_sync(fds, &n, max);
    return n;
}

void keys_take_back(const int *fds, int n)
{
    for (int i = 0; i < n; i++) {
        ioctl(fds[i], EVIOCREVOKE, NULL);
        close(fds[i]);
    }
}

int keys_fds(int *fds, int max)
{
    int n = count < max ? count : max;

    memcpy(fds, keyboards, sizeof(int) * (size_t)n);
    return n;
}

int keys_keyboards(struct keys_keyboard *out, int max)
{
    int n = 0;
    read_table();
    for (int i = 0; i < count && n < max; i++) {
        unsigned long bits[KEY_MAX / (8 * sizeof(long)) + 1] = { 0 };
        struct keys_keyboard *k = &out[n];
        memset(k, 0, sizeof *k);
        if (ioctl(keyboards[i], EVIOCGBIT(EV_KEY, sizeof bits), bits) < 0 || !has(bits, KEY_A) ||
            !has(bits, KEY_ENTER) || keys_device_id(keyboards[i], k->id, sizeof k->id) < 0) {
            continue;
        }
        if (ioctl(keyboards[i], EVIOCGNAME(sizeof k->name - 1), k->name) < 0) k->name[0] = '\0';
        int own = device_keymap(k->id, k->keymap, sizeof k->keymap) == 0;
        if (!own) snprintf(k->keymap, sizeof k->keymap, "%s", tables[0].layout_names[0]);
        k->own = own;
        k->missing = own && strcmp(k->keymap, tables[0].layout_names[0]) != 0 && device_table_of(i) == &tables[0];
        int seen = 0;
        for (int j = 0; j < n; j++) seen |= strcmp(out[j].id, k->id) == 0;
        if (!seen) n++;
    }
    return n;
}

int keys_set_keyboard(const char *id, const char *keymap)
{
    if (!id[0] || strpbrk(id, " \t\r\n") || (keymap && !keymap_name(keymap))) {
        errno = EINVAL;
        return -1;
    }
    char tmp[sizeof KEYS_DEVICES + 8], line[512], name[KEYS_ID_SIZE];
    snprintf(tmp, sizeof tmp, "%s.new", KEYS_DEVICES);
    FILE *in = fopen(KEYS_DEVICES, "re"), *out = fopen(tmp, "we");
    if (!out) {
        if (in) fclose(in);
        return -1;
    }
    if (!in) {
        fputs("# Keyboards with a keymap of their own (settings: Input & Output, Keyboard):\n"
              "# the keyboard as vendor:product:name, a console keymap as rc.conf's keymap.\n", out);
    }
    while (in && fgets(line, sizeof line, in)) {
        if (line[0] != '#' && sscanf(line, "%299s", name) == 1 && strcmp(name, id) == 0) continue;
        fputs(line, out);
    }
    if (keymap) fprintf(out, "%s %s\n", id, keymap);
    if (in) fclose(in);
    if (fclose(out) != 0 || rename(tmp, KEYS_DEVICES) != 0) {
        unlink(tmp);
        return -1;
    }
    return 0;
}

/* What chars types: the character itself when it is one in ASCII, else
 * K_TEXT with its UTF-8 (keys_text). */
static int text_key(const uint32_t *c, int n)
{
    if (n == 1 && c[0] < 0x80) return (int)c[0];
    size_t len = 0;
    for (int i = 0; i < n; i++) {
        uint32_t v = c[i];
        if (v < 0x80) {
            typed_text[len++] = (char)v;
        } else if (v < 0x800) {
            typed_text[len++] = (char)(0xC0 | v >> 6);
            typed_text[len++] = (char)(0x80 | (v & 0x3F));
        } else if (v < 0x10000) {
            typed_text[len++] = (char)(0xE0 | v >> 12);
            typed_text[len++] = (char)(0x80 | (v >> 6 & 0x3F));
            typed_text[len++] = (char)(0x80 | (v & 0x3F));
        } else {
            typed_text[len++] = (char)(0xF0 | v >> 18);
            typed_text[len++] = (char)(0x80 | (v >> 12 & 0x3F));
            typed_text[len++] = (char)(0x80 | (v >> 6 & 0x3F));
            typed_text[len++] = (char)(0x80 | (v & 0x3F));
        }
    }
    typed_text[len] = '\0';
    return K_TEXT;
}

/* A dead key (or Multi_key) begins one of the compose sequences. */
static int compose_starts(const struct table *tb, uint32_t sym)
{
    for (int i = 0; sym && i < tb->nsequences; i++) {
        if (tb->sequences[i].keys[0] == sym) return 1;
    }
    return 0;
}

/* The next key of the sequence being typed, as libxkbcommon's compose state
 * takes it: the sequence's text when it is complete, 0 while it goes on, and
 * 0 when no sequence goes this way: the sequence and that key are dropped
 * (XKB_COMPOSE_CANCELLED, as foot and GTK's front ends drop it). */
static int compose_feed(const struct table *tb, uint32_t sym)
{
    const struct sequence *done = NULL;
    int longer = 0;
    pending[npending++] = sym;
    for (int i = 0; i < tb->nsequences; i++) {
        const struct sequence *q = &tb->sequences[i];
        if (q->n < npending || memcmp(q->keys, pending, (size_t)npending * sizeof *pending) != 0) continue;
        if (q->n == npending) done = q;
        else longer = 1;
    }
    if (longer && npending < MAX_SEQUENCE) return 0;
    npending = 0;
    return done ? text_key(done->out, done->nout) : 0;
}

/* The key for a press (or repeat) of code on a keyboard with table tb, or 0
 * if it gives none. */
static int translate(struct table *tb, int code, int repeat)
{
    int act = match_action(tb, code, ctrl, alt, shift, meta);
    if (act != 0) {
        if ((act == K_ACT_HOME || act == K_ACT_LOCK || act == K_CTRL_SPACE || act == K_ACT_IME_CN ||
             act == K_ACT_IME_RAW || act == K_ACT_IME_WIDTH || act == K_VOL_MUTE || act == K_MIC_MUTE) && repeat) {
            return 0;
        }
        return act;
    }
    int bare = !ctrl && !alt && !meta && !name_any, st = (shift ? 1 : 0) | (caps ? 2 : 0) | (altgr ? 4 : 0);
    if (bare && npending > 0 && code > 0 && code < 256) {
        return compose_feed(tb, tb->syms[st][code]); /* any key goes on with the sequence, or ends it */
    }
    switch (code) {
    case KEY_UP:
        return K_UP;
    case KEY_DOWN:
        return K_DOWN;
    case KEY_LEFT:
        return K_LEFT;
    case KEY_RIGHT:
        return K_RIGHT;
    case KEY_PAGEUP:
        return K_PGUP;
    case KEY_PAGEDOWN:
        return K_PGDN;
    case KEY_HOME:
        return K_BOL;
    case KEY_END:
        return K_EOL;
    case KEY_DELETE:
        return K_DEL;
    case KEY_INSERT:
        return K_INS;
    case KEY_KPENTER:
        return '\n';
    case KEY_F11:
        return K_F1 + 10;
    case KEY_F12:
        return K_F1 + 11;
    }
    if (code >= KEY_F1 && code <= KEY_F10) {
        return K_F1 + code - KEY_F1;
    }
    if (code < 0 || code > 255) {
        return 0;
    }
    if (ctrl && !alt && !shift && code >= KEY_1 && code <= KEY_0) {
        return K_CTRL_0 + (code == KEY_0 ? 0 : code - KEY_1 + 1);
    }
    uint32_t c = tb->chars[st & ~2][code];
    if (ctrl && (((c | 0x20) >= 'a' && (c | 0x20) <= 'z') || (c >= '[' && c <= '_'))) {
        return (int)(c & 0x1F);
    }
    c = tb->chars[st][code];
    if (c == 0 && bare && compose_starts(tb, tb->syms[st][code])) {
        pending[0] = tb->syms[st][code];
        npending = 1;
        return 0;
    }
    if (c == 0 && name_any && tb->keysym[code][0] && !modifier_name(tb->keysym[code])) {
        return K_NAMED;
    }
    if (c < 0x80) {
        return (int)c;
    }
    return bare ? text_key(&c, 1) : 0; /* Ctrl+ö: none */
}

void keys_name_any(int on)
{
    name_any = on;
}

int keys_last_name(char *out, size_t size)
{
    char key[32];
    const struct table *tb = last_table;
    read_table();
    if (last_code == KEY_SPACE || last_code == KEY_ENTER || last_code == KEY_TAB) {
        snprintf(key, sizeof key, "%s", last_code == KEY_SPACE ? "Space" : last_code == KEY_ENTER ? "Enter" : "Tab");
    } else if (last_code > 0 && last_code < 256 && tb->chars[0][last_code] < 0x80 &&
               isalnum((int)tb->chars[0][last_code])) {
        snprintf(key, sizeof key, "%c", toupper((int)tb->chars[0][last_code]));
    } else if (last_code > 0 && last_code < 256 && tb->keysym[last_code][0] && !tb->chars[0][last_code] &&
               !modifier_name(tb->keysym[last_code]) && parse_key_name(tb, tb->keysym[last_code]) == last_code) {
        snprintf(key, sizeof key, "%s", tb->keysym[last_code]); /* Henkan_Mode */
    } else {
        return -1;
    }
    snprintf(out, size, "%s%s%s%s%s", ctrl ? "Ctrl+" : "", alt ? "Alt+" : "", shift ? "Shift+" : "",
             meta ? "Super+" : "", key);
    return 0;
}

int keys_modifiers(void)
{
    return (shift ? KEYS_SHIFT : 0) | (ctrl ? KEYS_CTRL : 0) | (alt ? KEYS_ALT : 0) | (meta ? KEYS_META : 0);
}

int keys_event(int fd)
{
    struct input_event ev;

    if (read(fd, &ev, sizeof ev) != sizeof ev || ev.type != EV_KEY || ev.value < 0 || ev.value > 2) {
        return 0;
    }
    int device = -1;
    for (int i = 0; i < count; i++) {
        if (keyboards[i] == fd) { device = i; break; }
    }
    read_table();
    struct table *tb = device >= 0 ? device_table_of(device) : &tables[0];
    int role = ev.code > 0 && ev.code < 256 ? tb->modifier_role[ev.code] : 0;
    if (!role && ev.code > 0 && ev.code < 256 && ev.value == 1 && tb->syms[shift ? 1 : 0][ev.code] == 0xffe5) {
        caps_code = ev.code; /* Caps_Lock at this level: the lock, as xkb has it */
    }
    if (!role && ev.code == caps_code && caps_code) {
        role = KEYS_CAPS;
        if (ev.value == 0) caps_code = 0;
    }
    if (role && role != KEYS_CAPS) {
        unsigned char *held = device >= 0 ? modifiers[device] : unowned_modifiers;
        held[ev.code] = ev.value != 0;
        update_modifiers();
        return ev.value == 1 ? match_action(tb, ev.code, ctrl, alt, shift, meta) : 0;
    }
    if (role == KEYS_CAPS) { /* locks at a press, unlocks at the next */
        if (ev.value != 2 && ime_caps_key) {
            ime_caps_key(ev.value == 1, caps);
        }
        if (ev.value == 1) {
            caps = !caps;
            if (ime_set_caps) {
                ime_set_caps(caps);
            }
        }
        return 0;
    }
    if (ev.value != 0) { /* 1 press, 2 repeat, 0 release */
        last_code = ev.code;
        last_table = tb;
        return translate(tb, ev.code, ev.value == 2);
    }
    return 0;
}

int key_poll_many(struct pollfd *extra, size_t nextra, int timeout_ms)
{
    size_t size = MAX_KEYBOARDS + 2 + nextra;
    struct pollfd *p = calloc(size, sizeof *p);
    if (!p) return 0;
    for (size_t i = 0; i < nextra; i++) extra[i].revents = 0;
    for (;;) {
        if (control_shown()) { free(p); return K_SHOWN; }
        if (pointer_next()) { free(p); return K_POINTER; }
        int n = 0, ctl = control_fd(), ctl_i = -1, ptr = pointer_fd(), ptr_i = -1;
        for (int i = 0; i < count; i++) {
            p[n++] = (struct pollfd){ .fd = keyboards[i], .events = POLLIN };
        }
        if (ctl >= 0) {
            ctl_i = n;
            p[n++] = (struct pollfd){ .fd = ctl, .events = POLLIN };
        }
        if (ptr >= 0) {
            ptr_i = n;
            p[n++] = (struct pollfd){ .fd = ptr, .events = POLLIN };
        }
        for (size_t i = 0; i < nextra; i++) p[n + i] = extra[i];
        int r = poll(p, (nfds_t)((size_t)n + nextra), timeout_ms);
        if (r < 0 && errno == EINTR) continue;
        if (r <= 0) { free(p); return 0; }
        if (ctl_i >= 0 && (p[ctl_i].revents & (POLLIN | POLLHUP))) {
            if (!control_service(NULL, 0, NULL)) {
                pause();
            }
            /* A take/release/pointer packet can replace extra input fds.
             * Return to the caller to rebuild its set, not to stale poll data. */
            free(p);
            return control_shown() ? K_SHOWN : 0;
        }
        if (ptr_i >= 0 && (p[ptr_i].revents & POLLIN)) {
            pointer_dispatch(); /* the cursor moves now; events below */
        }
        int ready = 0;
        for (size_t i = 0; i < nextra; i++) {
            extra[i].revents = p[n + i].revents;
            ready |= extra[i].revents != 0;
        }
        for (int i = 0; i < count; i++) {
            if (p[i].revents & (POLLERR | POLLHUP)) continue;
            int key = (p[i].revents & POLLIN) ? keys_event(keyboards[i]) : 0;
            if (key == K_VOL_UP || key == K_VOL_DOWN || key == K_VOL_MUTE || key == K_MIC_MUTE) {
                base_audio_key(key, NULL, 0);
                free(p); return 0;
            }
            if (key == K_BRI_UP || key == K_BRI_DOWN) {
                base_brightness_key(key, NULL, 0);
                free(p); return 0;
            }
            if (key != 0) { free(p); return key; }
        }
        if (pointer_next()) { free(p); return K_POINTER; }
        if (ready) { free(p); return 0; }
    }
}

int key_poll_timeout(int fd, int timeout_ms)
{
    struct pollfd p = { .fd = fd, .events = POLLIN };
    return key_poll_many(&p, 1, timeout_ms);
}

int key_poll(int fd)
{
    return key_poll_timeout(fd, -1);
}

int key_get(void)
{
    return key_poll(-1);
}
