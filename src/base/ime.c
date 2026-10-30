/*
 * base/ime.c: Rime input method with rime-frost + wanxiang grammar & English completion.
 * Supports 20 candidates per page: 1..9,0 for 1..10 and Ctrl+1..9,0 for 11..20.
 */
#ifdef CONFIG_IME

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include <rime_api.h>

#include "ime.h"
#include "base.h"

static RimeApi *rime;
static RimeSessionId session;
static int focused = 1;
static int current_mode = IME_MODE_RAW; /* Default: RAW passthrough */
static char commit_buf[512];
static const char *cn_schema = "rime_frost";
static int chinese_enabled = 1;
static char mode_labels[3][16] = { "RAW", "CN", "EN" };
static char mode_descriptions[3][64] = {
    " [RAW] Direct input ", " [CN] Pinyin ", " [EN] English completion "
};
/* Chinese mode's punctuation: full width each time it starts (the Chinese
 * punctuation of default.custom.yaml's table; other keys as typed), or half
 * width (ASCII): Rime's ascii_punct. Only punctuation:
 * letters, digits and space stay as typed (full_shape stays off). Shown after
 * the mode's label. */
static int full_width;
static char width_labels[2][16] = { "+Half", "+Full" };
static char width_descriptions[2][64] = { " [CN+Half] ASCII punctuation ", " [CN+Full] Chinese punctuation " };

static void mode_setting(const char *name, const char *value, void *arg)
{
    (void)arg;
    if (strcmp(name, "ime_chinese") == 0) {
        chinese_enabled = strcmp(value, "0") != 0 && strcmp(value, "no") != 0;
    }
}

#ifndef IME_MODE_LABELS
#define IME_MODE_LABELS "/usr/local/share/locachy/ime-mode-labels.conf"
#endif

static void load_mode_labels(void)
{
    FILE *in = fopen(IME_MODE_LABELS, "r");
    char line[256];
    while (in && fgets(line, sizeof line, in)) {
        line[strcspn(line, "\r\n")] = '\0';
        char *value = strchr(line, '=');
        if (!value || line[0] == '#') {
            continue;
        }
        *value++ = '\0';
        static const char *const names[] = { "raw", "cn", "en" };
        for (int i = 0; i < 3; i++) {
            char key[32];
            snprintf(key, sizeof key, "%s_label", names[i]);
            if (strcmp(line, key) == 0) {
                snprintf(mode_labels[i], sizeof mode_labels[i], "%s", value);
            }
            snprintf(key, sizeof key, "%s_description", names[i]);
            if (strcmp(line, key) == 0) {
                snprintf(mode_descriptions[i], sizeof mode_descriptions[i], "%s", value);
            }
        }
        if (strcmp(line, "half_label") == 0) {
            snprintf(width_labels[0], sizeof width_labels[0], "%s", value);
        } else if (strcmp(line, "full_label") == 0) {
            snprintf(width_labels[1], sizeof width_labels[1], "%s", value);
        } else if (strcmp(line, "half_description") == 0) {
            snprintf(width_descriptions[0], sizeof width_descriptions[0], "%s", value);
        } else if (strcmp(line, "full_description") == 0) {
            snprintf(width_descriptions[1], sizeof width_descriptions[1], "%s", value);
        }
    }
    if (in) {
        fclose(in);
    }
    rc_read(mode_setting, NULL);
}

static void set_width(int full)
{
    full_width = full;
    rime->set_option(session, "ascii_punct", full ? False : True);
    rime->set_option(session, "full_shape", False);
}

static void disable_emoji(void)
{
    if (rime && session && rime->set_option) {
        rime->set_option(session, "emoji", False);
        rime->set_option(session, "emoji_suggestion", False);
    }
}

int ime_init(const char *shared_dir, const char *user_dir)
{
    load_mode_labels();
    rime = rime_get_api();
    if (!rime) {
        return -1;
    }
    if (access("/var/lib/locachy/rime/build/context_pinyin.schema.yaml", R_OK) == 0 ||
        access("/var/lib/locachy/rime/context_pinyin.schema.yaml", R_OK) == 0) {
        cn_schema = "context_pinyin";
    } else if (access("/var/lib/locachy/rime/build/rime_frost.schema.yaml", R_OK) == 0 ||
               access("/usr/share/rime-data/rime_frost.schema.yaml", R_OK) == 0) {
        cn_schema = "rime_frost";
    } else if (access("/usr/share/rime-data/pinyin_simp.schema.yaml", R_OK) == 0) {
        cn_schema = "pinyin_simp";
    } else if (access("/usr/share/rime-data/luna_pinyin_simp.schema.yaml", R_OK) == 0) {
        cn_schema = "luna_pinyin_simp";
    } else {
        /* Only librime + words (English completion) is installed: no CN mode. */
        chinese_enabled = 0;
        cn_schema = "easy_en";
    }
    RIME_STRUCT(RimeTraits, traits);
    traits.shared_data_dir = shared_dir ? shared_dir : "/usr/share/rime-data";
    traits.user_data_dir = user_dir ? user_dir : "/var/lib/locachy/rime";
    traits.distribution_name = "Rime";
    traits.distribution_code_name = "locachy";
    traits.distribution_version = "1.0";
    traits.app_name = "rime.locachy";
    traits.min_log_level = 2; // 2 = ERROR

    rime->setup(&traits);
    rime->initialize(&traits);
    char build_default[256];
    snprintf(build_default, sizeof build_default, "%s/build/default.yaml",
             traits.user_data_dir);
    if (access(build_default, R_OK) != 0) {
        rime->start_maintenance(False);
        rime->join_maintenance_thread();
    }

    session = rime->create_session();
    if (!session) {
        return -1;
    }
    rime->select_schema(session, chinese_enabled ? cn_schema : "easy_en");
    if (chinese_enabled) {
        disable_emoji();
    }
    current_mode = IME_MODE_RAW;
    return 0;
}

void ime_done(void)
{
    if (rime && session) {
        rime->destroy_session(session);
        rime->finalize();
        session = 0;
        rime = NULL;
    }
}

void ime_set_focus(int f)
{
    focused = f != 0;
    if (!focused && rime && session) {
        rime->clear_composition(session);
    }
}

int ime_has_focus(void)
{
    return focused;
}

void ime_set_mode(int mode)
{
    if (mode < IME_MODE_RAW || mode > IME_MODE_EN ||
        (mode == IME_MODE_CN && !chinese_enabled)) {
        return;
    }
    if (!rime || !session) {
        current_mode = mode;
        return;
    }
    if (mode == current_mode) {
        return;
    }
    rime->clear_composition(session);
    current_mode = mode;
    if (current_mode == IME_MODE_CN) {
        rime->select_schema(session, cn_schema);
        disable_emoji();
        set_width(1);
    } else if (current_mode == IME_MODE_EN) {
        rime->select_schema(session, "easy_en");
    }
}

int ime_cycle_mode(void)
{
    int next;
    if (current_mode == IME_MODE_RAW) {
        next = chinese_enabled ? IME_MODE_CN : IME_MODE_EN;
    } else if (current_mode == IME_MODE_CN) {
        next = IME_MODE_EN;
    } else {
        next = IME_MODE_RAW;
    }
    ime_set_mode(next);
    return current_mode;
}

int ime_mode(void)
{
    return current_mode;
}

const char *ime_mode_label(void)
{
    static char label[40];
    if (current_mode == IME_MODE_CN) {
        snprintf(label, sizeof label, "%.15s%.15s", mode_labels[current_mode], width_labels[full_width]);
        return label;
    }
    return mode_labels[current_mode];
}

const char *ime_toggle_width(void)
{
    if (!rime || !session || current_mode != IME_MODE_CN) {
        return NULL;
    }
    rime->clear_composition(session);
    set_width(!full_width);
    return width_descriptions[full_width];
}

/* rc.conf's key_ime_width_cn, as locachy-bar reads it: keys separated by
 * blanks or commas, each its modifiers and a key joined by +, the key a
 * character or Space. Read again when rc.conf changes. */
#define MAX_WIDTH_KEYS 8
static struct { int ch, mods; } width_keys[MAX_WIDTH_KEYS];
static int nwidth_keys = -1;
static struct timespec width_keys_read;

static void on_width_rc(const char *name, const char *value, void *arg)
{
    if (strcmp(name, "key_ime_width_cn") == 0) snprintf(arg, 128, "%s", value);
}

static void load_width_keys(void)
{
    struct stat st = { 0 }; /* no rc.conf: the defaults, read each time */
    if (stat(RC_CONF, &st) == 0 && nwidth_keys >= 0 && st.st_mtim.tv_sec == width_keys_read.tv_sec &&
        st.st_mtim.tv_nsec == width_keys_read.tv_nsec) {
        return;
    }
    width_keys_read = st.st_mtim;
    char spec[128] = "Ctrl+. Shift+Space", *save = NULL;
    rc_read(on_width_rc, spec);
    for (char *p = spec; *p; p++) {
        if (*p == ',') *p = ' ';
    }
    nwidth_keys = 0;
    for (char *tok = strtok_r(spec, " \t", &save); tok && nwidth_keys < MAX_WIDTH_KEYS;
         tok = strtok_r(NULL, " \t", &save)) {
        int ch = 0, mods = 0;
        char *part_save = NULL;
        for (char *part = strtok_r(tok, "+", &part_save); part; part = strtok_r(NULL, "+", &part_save)) {
            if (strcasecmp(part, "Ctrl") == 0 || strcasecmp(part, "Control") == 0) mods |= KEYS_CTRL;
            else if (strcasecmp(part, "Alt") == 0) mods |= KEYS_ALT;
            else if (strcasecmp(part, "Shift") == 0) mods |= KEYS_SHIFT;
            else if (strcasecmp(part, "Super") == 0 || strcasecmp(part, "Meta") == 0 ||
                     strcasecmp(part, "Win") == 0) mods |= KEYS_META;
            else if (strcasecmp(part, "Space") == 0) ch = ' ';
            else if (!part[1]) ch = tolower((unsigned char)part[0]);
        }
        if (ch) {
            width_keys[nwidth_keys].ch = ch;
            width_keys[nwidth_keys++].mods = mods;
        }
    }
}

int ime_width_key(int key, int mods)
{
    load_width_keys();
    mods &= KEYS_CTRL | KEYS_ALT | KEYS_SHIFT | KEYS_META;
    for (int i = 0; i < nwidth_keys; i++) {
        if (key >= 0 && key < 128 && tolower(key) == width_keys[i].ch && mods == width_keys[i].mods) return 1;
    }
    return 0;
}

const char *ime_mode_desc(void)
{
    return mode_descriptions[current_mode];
}

int ime_action(int key, const char **notice)
{
    *notice = NULL;
    if (key == K_CTRL_SPACE) {
        ime_cycle_mode();
    } else if (key == K_ACT_IME_CN) {
        ime_set_mode(chinese_enabled ? IME_MODE_CN : IME_MODE_EN);
    } else if (key == K_ACT_IME_RAW) {
        ime_set_mode(IME_MODE_RAW);
    } else if (key == K_ACT_IME_WIDTH) {
        *notice = ime_toggle_width(); /* NULL outside Chinese mode */
        return 1;
    } else {
        return 0;
    }
    *notice = ime_mode_desc();
    return 1;
}

static int menu_count(void)
{
    if (!rime || !session) {
        return -1;
    }
    RIME_STRUCT(RimeContext, ctx);
    int n = -1;
    if (rime->get_context(session, &ctx)) {
        if (ctx.composition.preedit && ctx.composition.preedit[0] != '\0') {
            n = ctx.menu.num_candidates;
        }
        rime->free_context(&ctx);
    }
    return n;
}

/* Rime's input has a letter: pinyin (or English) being typed, whose menu the
 * digits choose from. Without one it is a digit separator waiting ("3." of
 * 3.14: librime's punctuator/digit_separators), which takes the digits. */
static int input_has_letter(void)
{
    const char *in = rime->get_input(session);
    for (; in && *in; in++) {
        if ((*in >= 'a' && *in <= 'z') || (*in >= 'A' && *in <= 'Z')) {
            return 1;
        }
    }
    return 0;
}

static void collect_commit(void)
{
    RIME_STRUCT(RimeCommit, commit);
    if (rime->get_commit(session, &commit)) {
        if (commit.text && commit.text[0]) {
            snprintf(commit_buf, sizeof commit_buf, "%s", commit.text);
        }
        rime->free_commit(&commit);
    }
}

/* Rime's key masks (librime's key_table.h, X11's): Caps Lock locked, a
 * release; Caps Lock's keysym. */
#define RIME_LOCK_MASK    (1 << 1)
#define RIME_RELEASE_MASK (1 << 30)
#define RIME_CAPS_LOCK    0xffe5 // XK_Caps_Lock
static int caps_locked;

void ime_caps_key(int pressed, int locked)
{
    ime_key_event(RIME_CAPS_LOCK, (locked ? RIME_LOCK_MASK : 0) | (pressed ? 0 : RIME_RELEASE_MASK));
}

void ime_set_caps(int locked)
{
    caps_locked = locked != 0;
}

static int to_keysym(int key)
{
    switch (key) {
    case '\b':       return 0xff08; // XK_BackSpace
    case '\t':       return 0xff09; // XK_Tab
    case '\n':       return 0xff0d; // XK_Return
    case 27:         return 0xff1b; // XK_Escape
    case K_LEFT:     return 0xff51; // XK_Left
    case K_UP:       return 0xff52; // XK_Up
    case K_RIGHT:    return 0xff53; // XK_Right
    case K_DOWN:     return 0xff54; // XK_Down
    case K_PGUP:     return 0xff55; // XK_Page_Up
    case K_PGDN:     return 0xff56; // XK_Page_Down
    case K_DEL:      return 0xffff; // XK_Delete
    case K_BOL:      return 0xff50; // XK_Home
    case K_EOL:      return 0xff57; // XK_End
    default:         return key;
    }
}

int ime_process_key(int key)
{
    if (!rime || !session || !focused || current_mode == IME_MODE_RAW) {
        return 0; // 100% Passthrough
    }

    int ncands = menu_count();
    int composing = (ncands >= 0);

    // Candidate selection when composing:
    //   1..9, 0       -> candidates 1..10 (index 0..9)
    //   Ctrl+1..9, 0  -> candidates 11..20 (index 10..19)
    if (composing && ncands > 0 && input_has_letter()) {
        int idx = -1;
        if (key >= '1' && key <= '9') {
            idx = key - '1';
        } else if (key == '0') {
            idx = 9;
        } else if (key >= K_CTRL_0 && key <= K_CTRL_0 + 9) {
            int digit = key - K_CTRL_0;
            idx = 10 + (digit == 0 ? 9 : digit - 1);
        }
        if (idx >= 0) {
            if (idx < ncands && rime->select_candidate_on_current_page) {
                rime->select_candidate_on_current_page(session, (size_t)idx);
                collect_commit();
            }
            return 1;
        }
    }

    // A character beyond ASCII (ä, a dead key's é): the program types it;
    // Rime has no key for it, and its composition stays.
    if (key == K_TEXT) {
        return 0;
    }

    // Other control combinations (Ctrl+C, Ctrl+D, Ctrl+L, Alt+X, etc.):
    // If user presses a combo while composing, abort composition and pass through!
    if ((key < 0x20 && key != '\b' && key != '\t' && key != '\n' && key != 27) ||
        (key >= K_CTRL_0 && key <= K_CTRL_0 + 9) || key >= 0x200) {
        if (composing) {
            rime->clear_composition(session);
        }
        return 0;
    }

    // In Chinese mode every key goes to Rime, as in any Rime front end: it
    // takes its own (letters; at full width the punctuation of
    // default.custom.yaml's table, but not "." or ":" right after a digit:
    // 3.14, 12:30, and the key again gives the full-width stop or colon)
    // and gives back the others (digits, space, Enter, the rest), which go on
    // as typed. It has to see them to know what came before. In English mode,
    // when not composing, only letters go to it.
    if (!composing && current_mode != IME_MODE_CN) {
        int is_letter = ((key >= 'a' && key <= 'z') || (key >= 'A' && key <= 'Z'));
        if (!is_letter) {
            return 0; // Passthrough to shell/app directly!
        }
    }

    return ime_key_event((unsigned int)to_keysym(key), caps_locked ? RIME_LOCK_MASK : 0);
}

int ime_key_event(unsigned int sym, unsigned int mask)
{
    if (!rime || !session || !focused || current_mode == IME_MODE_RAW || !sym) return 0;
    /* EN keeps punctuation and navigation direct when no composition is
     * active; lock/modifier events still reach ascii_composer. */
    if (current_mode == IME_MODE_EN && menu_count() < 0 && !(mask & RIME_RELEASE_MASK) &&
        !((sym >= 'a' && sym <= 'z') || (sym >= 'A' && sym <= 'Z')) &&
        !(sym >= 0xffe1 && sym <= 0xffee) && sym != 0xff7f && sym != 0xff30) return 0;
    /* fcitx5-rime/IBus use these X11 modifiers and the release flag. The
     * resolved keysym already contains Num Lock and the keyboard layout. */
    mask &= (1u << 0) | (1u << 1) | (1u << 2) | (1u << 3) | (1u << 6) | (1u << 26) | (1u << 30);
    if (mask & (1u << 6)) mask |= 1u << 26; /* IBus virtual Super */
    Bool handled = rime->process_key(session, (int)sym, (int)mask);
    collect_commit();
    return handled != 0;
}

const char *ime_get_commit(void)
{
    if (commit_buf[0] == '\0') {
        return NULL;
    }
    static char out[512];
    snprintf(out, sizeof out, "%s", commit_buf);
    commit_buf[0] = '\0';
    return out;
}

static int is_wide_cp(uint32_t cp)
{
    return (cp >= 0x1100 && cp <= 0x11FF) ||
           (cp >= 0x2E80 && cp <= 0xA4CF) ||
           (cp >= 0xAC00 && cp <= 0xD7AF) ||
           (cp >= 0xF900 && cp <= 0xFAFF) ||
           (cp >= 0xFE10 && cp <= 0xFE4F) ||
           (cp >= 0xFF01 && cp <= 0xFF60) ||
           (cp >= 0xFFE0 && cp <= 0xFFE6) ||
           (cp >= 0x1F300 && cp <= 0x1FAFF) ||
           (cp >= 0x20000 && cp <= 0x2FA1F);
}

static int str_cells(const char *s)
{
    int w = 0;
    const uint8_t *p = (const uint8_t *)s, *end = p + strlen(s);
    while (p < end) {
        uint8_t b = *p++;
        int n = (b < 0x80) ? 0 : (b & 0xE0) == 0xC0 ? 1 : (b & 0xF0) == 0xE0 ? 2 : (b & 0xF8) == 0xF0 ? 3 : 0;
        uint32_t cp = b & (0x3Fu >> n);
        for (int i = 0; i < n && p < end; i++) {
            cp = (cp << 6) | (*p++ & 0x3F);
        }
        w += is_wide_cp(cp) ? 2 : 1;
    }
    return w;
}

static size_t append_padded(char *dst, size_t size, size_t pos, const char *s, int target_cells)
{
    if (pos >= size) {
        return pos;
    }
    size_t slen = strlen(s);
    if (pos + slen < size) {
        memcpy(dst + pos, s, slen);
        pos += slen;
    }
    int pad = target_cells - str_cells(s);
    while (pad-- > 0 && pos + 1 < size) {
        dst[pos++] = ' ';
    }
    dst[pos] = '\0';
    return pos;
}

int ime_get_overlay(char *row1, char *row2, size_t size)
{
    row1[0] = row2[0] = '\0';
    if (!rime || !session || current_mode == IME_MODE_RAW) {
        return 0;
    }
    RIME_STRUCT(RimeContext, ctx);
    if (!rime->get_context(session, &ctx)) {
        return 0;
    }
    int active = 0;
    if (ctx.composition.preedit && ctx.composition.preedit[0]) {
        active = 1;
        int ncands = ctx.menu.num_candidates;
        int has_row2 = (ncands > 10);
        const char *tag = ime_mode_label();

        char hdr1[128], hdr2[128];
        snprintf(hdr1, sizeof hdr1, "[%s p.%d] %s", tag, ctx.menu.page_no + 1, ctx.composition.preedit);
        snprintf(hdr2, sizeof hdr2, "Ctrl+1..0");

        int w_hdr1 = str_cells(hdr1);
        int w_hdr2 = has_row2 ? str_cells(hdr2) : 0;
        int w_hdr = (w_hdr1 > w_hdr2) ? w_hdr1 : w_hdr2;

        size_t l1 = append_padded(row1, size, 0, hdr1, w_hdr);
        l1 = append_padded(row1, size, l1, " | ", 3);

        size_t l2 = 0;
        if (has_row2) {
            l2 = append_padded(row2, size, 0, hdr2, w_hdr);
            l2 = append_padded(row2, size, l2, " | ", 3);
        }

        int limit1 = ncands < 10 ? ncands : 10;
        for (int i = 0; i < limit1 && l1 < size; i++) {
            int label = (i + 1) % 10;
            int hl1 = (i == ctx.menu.highlighted_candidate_index);
            char item1[128], item2[128];
            snprintf(item1, sizeof item1, hl1 ? "[%d.%s]" : " %d.%s ",
                     label, ctx.menu.candidates[i].text);

            int w1 = str_cells(item1);
            int w2 = 0;
            item2[0] = '\0';
            if (has_row2 && (10 + i) < ncands) {
                int hl2 = ((10 + i) == ctx.menu.highlighted_candidate_index);
                snprintf(item2, sizeof item2, hl2 ? "[^%d.%s]" : "^%d.%s ",
                         label, ctx.menu.candidates[10 + i].text);
                w2 = str_cells(item2);
            }
            int col_w = (w1 > w2 ? w1 : w2) + 1;
            l1 = append_padded(row1, size, l1, item1, col_w);
            if (has_row2 && item2[0] != '\0') {
                l2 = append_padded(row2, size, l2, item2, col_w);
            }
        }
    }
    rime->free_context(&ctx);
    return active;
}

#endif /* CONFIG_IME */
