/*
 * The keyboard through evdev, plus configurable global keybindings from
 * /etc/locachy/rc.conf (key_home, key_lock, key_ime, key_ime_cn, key_ime_raw,
 * key_ime_width, key_volume_up, key_volume_down, key_volume_mute,
 * key_mic_mute). Keys type as the layout's key table has them (keytable):
 * with Shift, Caps Lock and AltGr, any character (K_TEXT beyond ASCII), and
 * the dead keys' sequences of libxkbcommon's compose table.
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
#define NSTATES 8
#define MAX_SEQUENCE 6
#define MAX_RESULT 4
static uint32_t syms[NSTATES][256], chars[NSTATES][256];
static char keysym[256][24];
static unsigned char altgr_key[256];
struct sequence {
    uint32_t keys[MAX_SEQUENCE], out[MAX_RESULT];
    unsigned char n, nout;
};
static struct sequence *sequences;
static int nsequences;
static uint32_t pending[MAX_SEQUENCE]; /* the dead keys' sequence being typed */
static int npending;
static char typed_text[4 * MAX_RESULT + 1]; /* K_TEXT's */
static char layout_names[5][64]; /* console, layout, model, variant, options */
static const char *const layout_fields[5] = { "console", "layout", "model", "variant", "options" };
static int table_read;

struct binding {
    int code;
    uint8_t ctrl, alt, shift, meta;
    int action;
};

static struct binding bindings[MAX_BINDINGS];
static int nbindings;
static int bindings_loaded;

static int keyboards[MAX_KEYBOARDS];
static char paths[MAX_KEYBOARDS][32]; /* process 1: to open them again */
static int count;
/* Left/right Shift, Ctrl, Alt, Super per device; 0x100 another AltGr key. */
static unsigned short modifiers[MAX_KEYBOARDS];
static int shift, ctrl, alt, meta, altgr;
static int caps; /* Caps Lock locked: letters in capitals (with Shift, small) */
static int last_code; /* the last key pressed, not a modifier */
static int name_any;  /* keys_name_any */

/* The input method (ime.c), in the programs that have it: Caps Lock's key
 * and state (Rime passes keys by while it is locked). */
__attribute__((weak)) void ime_caps_key(int pressed, int locked);
__attribute__((weak)) void ime_set_caps(int locked);

static int has(const unsigned long *bits, int n)
{
    return (bits[n / (8 * sizeof(long))] >> (n % (8 * sizeof(long)))) & 1;
}

static void add_binding(int code, int c, int a, int s, int m, int action)
{
    if (nbindings >= MAX_BINDINGS) {
        return;
    }
    bindings[nbindings++] = (struct binding){
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
static void us_states(void)
{
    for (size_t i = 0; i < sizeof normal - 1; i++) {
        uint32_t n = (unsigned char)normal[i], u = (unsigned char)shifted[i];
        int letter = isalpha((int)n);
        for (int st = 0; st < NSTATES; st++) {
            int up = !!(st & 1) != (letter && (st & 2));
            chars[st][i] = up ? u : n;
        }
    }
}

int keys_layout_load(const char *path)
{
    table_read = 1;
    bindings_loaded = 0; /* names resolve through the new table */
    memset(syms, 0, sizeof syms);
    memset(chars, 0, sizeof chars);
    memset(keysym, 0, sizeof keysym);
    memset(altgr_key, 0, sizeof altgr_key);
    memset(layout_names, 0, sizeof layout_names);
    free(sequences);
    sequences = NULL;
    nsequences = npending = 0;
    us_states();
    FILE *f = path ? fopen(path, "re") : NULL;
    if (!f) return -1;
    static uint32_t got_syms[NSTATES][256], got_chars[NSTATES][256];
    memset(got_syms, 0, sizeof got_syms);
    memset(got_chars, 0, sizeof got_chars);
    char names[256][24] = { { 0 } }, line[512];
    int keys = 0, size = 0;
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
        } else if (sscanf(line, "altgr %d", &code) == 1) {
            if (code > 0 && code < 256) altgr_key[code] = 1;
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
            if (nsequences == size) {
                size = size ? 2 * size : 256;
                struct sequence *more = realloc(sequences, (size_t)size * sizeof *sequences);
                if (!more) continue;
                sequences = more;
            }
            sequences[nsequences++] = q;
        } else if (sscanf(line, "%15s %63s", field, value) == 2) {
            for (int i = 0; i < 5; i++) {
                if (strcmp(field, layout_fields[i]) == 0 && strcmp(value, "-") != 0) {
                    snprintf(layout_names[i], sizeof layout_names[i], "%s", value);
                }
            }
        }
    }
    fclose(f);
    if (keys == 0) { /* US, as without a table */
        memset(altgr_key, 0, sizeof altgr_key);
        free(sequences);
        sequences = NULL;
        nsequences = 0;
        return -1;
    }
    memcpy(syms, got_syms, sizeof syms);
    memcpy(chars, got_chars, sizeof chars);
    memcpy(keysym, names, sizeof keysym);
    return 0;
}

static void read_table(void)
{
    if (!table_read) keys_layout_load(KEYS_TABLE);
}

const char *keys_layout_name(const char *field)
{
    read_table();
    for (int i = 0; i < 5; i++) {
        if (strcmp(field, layout_fields[i]) == 0) return layout_names[i][0] ? layout_names[i] : NULL;
    }
    return NULL;
}

int keys_char(int code, int with_shift)
{
    read_table();
    uint32_t c = code > 0 && code < 256 ? chars[!!with_shift][code] : 0;
    return c < 0x80 ? (int)c : 0;
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

static int parse_key_name(const char *name)
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
            if (chars[0][i] == ch) {
                return i;
            }
        }
    }
    /* The layout's keys without a character, by keysym name (Henkan_Mode). */
    for (int i = 1; i < 256; i++) {
        if (keysym[i][0] && !chars[0][i] && strcasecmp(keysym[i], name) == 0) {
            return i;
        }
    }
    return -1;
}

static void parse_binding_spec(const char *spec, int action)
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
                code = parse_key_name(part);
            }
            part = plus ? plus + 1 : NULL;
        }
        if (code == -1 && (c || a || s || m)) {
            add_binding(0, c, a, s, m, action);
        } else if (code >= 0) {
            add_binding(code, c, a, s, m, action);
            if (code == KEY_VENDOR) {
                add_binding(KEY_PROG1, c, a, s, m, action);
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

static void load_keybindings(void)
{
    if (bindings_loaded) {
        return;
    }
    read_table();
    bindings_loaded = 1;
    /* The input method's keys on a JIS keyboard, by keysym name (keyboards
     * without these keys bind nothing): Henkan, right of the space bar,
     * Chinese; Muhenkan, left of it, direct input; Zenkaku_Hankaku, left of
     * 1 in the jp layout, the width of Chinese mode's punctuation. */
    struct key_cfg cfg = {
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
    rc_read(on_key_rc, &cfg);
    nbindings = 0;
    parse_binding_spec(cfg.home, K_ACT_HOME);
    parse_binding_spec(cfg.lock, K_ACT_LOCK);
    parse_binding_spec(cfg.ime, K_CTRL_SPACE);
    parse_binding_spec(cfg.ime_cn, K_ACT_IME_CN);
    parse_binding_spec(cfg.ime_raw, K_ACT_IME_RAW);
    parse_binding_spec(cfg.ime_width, K_ACT_IME_WIDTH);
    parse_binding_spec(cfg.vol_up, K_VOL_UP);
    parse_binding_spec(cfg.vol_down, K_VOL_DOWN);
    parse_binding_spec(cfg.vol_mute, K_VOL_MUTE);
    parse_binding_spec(cfg.mic_mute, K_MIC_MUTE);
    parse_binding_spec(cfg.bri_up, K_BRI_UP);
    parse_binding_spec(cfg.bri_down, K_BRI_DOWN);
}

void keys_reload(void)
{
    bindings_loaded = 0;
    table_read = 0;
    npending = 0;
}

int keys_match_action(int ev_code, int c, int a, int s, int m)
{
    load_keybindings();
    int is_mod_key = (ev_code == KEY_LEFTCTRL || ev_code == KEY_RIGHTCTRL ||
                      ev_code == KEY_LEFTALT || ev_code == KEY_RIGHTALT ||
                      ev_code == KEY_LEFTSHIFT || ev_code == KEY_RIGHTSHIFT ||
                      ev_code == KEY_LEFTMETA || ev_code == KEY_RIGHTMETA);
    for (int i = 0; i < nbindings; i++) {
        const struct binding *b = &bindings[i];
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

int keys_open(void)
{
    load_keybindings();
    if (control_fd() >= 0) {
        control_wait_take(); /* keys_take does the rest */
        pointer_open();      /* and the pointer comes after each take */
        return count;
    }
    keys_refresh();
    return count;
}

/* Right Alt is AltGr where the layout makes it so (de, fr, gb), not Alt:
 * AltGr+Q types @ and is no Alt binding. */
static void update_modifiers(void)
{
    unsigned short held = 0;
    for (int i = 0; i < count; i++) held |= modifiers[i];
    shift = !!(held & 0x03);
    ctrl = !!(held & 0x0c);
    alt = !!(held & (altgr_key[KEY_RIGHTALT] ? 0x10 : 0x30));
    meta = !!(held & 0xc0);
    altgr = (held & 0x100) || (altgr_key[KEY_RIGHTALT] && (held & 0x20));
}

/* Preserve live queues: reopening every device on an unrelated uevent loses
 * unread keys, and a path alone cannot distinguish a reconnected device. */
void keys_refresh(void)
{
    load_keybindings();
    for (int i = 0; i < count;) {
        int version;
        if (ioctl(keyboards[i], EVIOCGVERSION, &version) >= 0) { i++; continue; }
        close(keyboards[i]);
        count--;
        memmove(keyboards + i, keyboards + i + 1, (size_t)(count - i) * sizeof *keyboards);
        memmove(paths + i, paths + i + 1, (size_t)(count - i) * sizeof *paths);
        memmove(modifiers + i, modifiers + i + 1, (size_t)(count - i) * sizeof *modifiers);
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
            for (int b = 0; !wanted && b < nbindings; b++) {
                if (bindings[b].code >= KEY_MUTE && has(bits, bindings[b].code)) {
                    wanted = 1;
                }
            }
        }
        if (wanted) {
            snprintf(paths[count], sizeof paths[count], "%s", path);
            modifiers[count] = 0;
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
    unsigned short old_modifiers[MAX_KEYBOARDS];
    memcpy(old, keyboards, (size_t)old_count * sizeof *old);
    memcpy(old_modifiers, modifiers, (size_t)old_count * sizeof *modifiers);
    count = 0;
    for (int i = 0; i < n; i++) {
        if (count == MAX_KEYBOARDS) { close(fds[i]); continue; }
        int k;
        for (k = 0; k < old_count; k++) {
            if (!used[k] && input_same(old[k], fds[i])) break;
        }
        if (k < old_count) {
            keyboards[count] = old[k];
            modifiers[count++] = old_modifiers[k];
            used[k] = 1;
            close(fds[i]);
        } else {
            keyboards[count] = fds[i];
            modifiers[count++] = 0;
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
static int compose_starts(uint32_t sym)
{
    for (int i = 0; sym && i < nsequences; i++) {
        if (sequences[i].keys[0] == sym) return 1;
    }
    return 0;
}

/* The next key of the sequence being typed, as libxkbcommon's compose state
 * takes it: the sequence's text when it is complete, 0 while it goes on, and
 * 0 when no sequence goes this way: the sequence and that key are dropped
 * (XKB_COMPOSE_CANCELLED, as foot and GTK's front ends drop it). */
static int compose_feed(uint32_t sym)
{
    const struct sequence *done = NULL;
    int longer = 0;
    pending[npending++] = sym;
    for (int i = 0; i < nsequences; i++) {
        const struct sequence *q = &sequences[i];
        if (q->n < npending || memcmp(q->keys, pending, (size_t)npending * sizeof *pending) != 0) continue;
        if (q->n == npending) done = q;
        else longer = 1;
    }
    if (longer && npending < MAX_SEQUENCE) return 0;
    npending = 0;
    return done ? text_key(done->out, done->nout) : 0;
}

/* The key for a press (or repeat) of code, or 0 if it gives none. */
static int translate(int code, int repeat)
{
    int act = keys_match_action(code, ctrl, alt, shift, meta);
    if (act != 0) {
        if ((act == K_ACT_HOME || act == K_ACT_LOCK || act == K_CTRL_SPACE || act == K_ACT_IME_CN ||
             act == K_ACT_IME_RAW || act == K_ACT_IME_WIDTH || act == K_VOL_MUTE || act == K_MIC_MUTE) && repeat) {
            return 0;
        }
        return act;
    }
    int bare = !ctrl && !alt && !meta && !name_any, st = (shift ? 1 : 0) | (caps ? 2 : 0) | (altgr ? 4 : 0);
    if (bare && npending > 0 && code > 0 && code < 256) {
        return compose_feed(syms[st][code]); /* any key goes on with the sequence, or ends it */
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
    uint32_t c = chars[st & ~2][code];
    if (ctrl && (((c | 0x20) >= 'a' && (c | 0x20) <= 'z') || (c >= '[' && c <= '_'))) {
        return (int)(c & 0x1F);
    }
    c = chars[st][code];
    if (c == 0 && bare && compose_starts(syms[st][code])) {
        pending[0] = syms[st][code];
        npending = 1;
        return 0;
    }
    if (c == 0 && name_any && keysym[code][0] && !modifier_name(keysym[code])) {
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
    read_table();
    if (last_code == KEY_SPACE || last_code == KEY_ENTER || last_code == KEY_TAB) {
        snprintf(key, sizeof key, "%s", last_code == KEY_SPACE ? "Space" : last_code == KEY_ENTER ? "Enter" : "Tab");
    } else if (last_code > 0 && last_code < 256 && chars[0][last_code] < 0x80 && isalnum((int)chars[0][last_code])) {
        snprintf(key, sizeof key, "%c", toupper((int)chars[0][last_code]));
    } else if (last_code > 0 && last_code < 256 && keysym[last_code][0] && !chars[0][last_code] &&
               !modifier_name(keysym[last_code]) && parse_key_name(keysym[last_code]) == last_code) {
        snprintf(key, sizeof key, "%s", keysym[last_code]); /* Henkan_Mode */
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

    if (read(fd, &ev, sizeof ev) != sizeof ev || ev.type != EV_KEY) {
        return 0;
    }
    read_table();
    static const int mod_codes[] = { KEY_LEFTSHIFT, KEY_RIGHTSHIFT, KEY_LEFTCTRL, KEY_RIGHTCTRL,
                                     KEY_LEFTALT, KEY_RIGHTALT, KEY_LEFTMETA, KEY_RIGHTMETA };
    int level3 = ev.code < 256 && altgr_key[ev.code] && ev.code != KEY_RIGHTALT; /* AltGr on another key */
    for (int i = 0; i < count; i++) {
        if (keyboards[i] != fd) continue;
        if (level3) {
            if (ev.value) modifiers[i] |= 0x100;
            else modifiers[i] &= (unsigned short)~0x100u;
            update_modifiers();
            return 0;
        }
        for (int k = 0; k < 8; k++) {
            if (ev.code != mod_codes[k]) continue;
            if (ev.value) modifiers[i] |= (unsigned short)(1u << k);
            else modifiers[i] &= (unsigned short)~(1u << k);
            update_modifiers();
            return ev.value == 1 ? keys_match_action(ev.code, ctrl, alt, shift, meta) : 0;
        }
        break;
    }
    int is_mod = 0;
    if (level3 || (ev.code == KEY_RIGHTALT && altgr_key[KEY_RIGHTALT])) {
        altgr = ev.value != 0;
        return 0;
    } else if (ev.code == KEY_LEFTSHIFT || ev.code == KEY_RIGHTSHIFT) {
        shift = ev.value != 0;
        is_mod = 1;
    } else if (ev.code == KEY_LEFTCTRL || ev.code == KEY_RIGHTCTRL) {
        ctrl = ev.value != 0;
        is_mod = 1;
    } else if (ev.code == KEY_LEFTALT || ev.code == KEY_RIGHTALT) {
        alt = ev.value != 0;
        is_mod = 1;
    } else if (ev.code == KEY_LEFTMETA || ev.code == KEY_RIGHTMETA) {
        meta = ev.value != 0;
        is_mod = 1;
    }
    if (is_mod) {
        if (ev.value == 1) {
            return keys_match_action(ev.code, ctrl, alt, shift, meta);
        }
        return 0;
    }
    if (ev.code == KEY_CAPSLOCK) { /* locks at a press, unlocks at the next */
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
        return translate(ev.code, ev.value == 2);
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
