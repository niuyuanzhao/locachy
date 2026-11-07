/* base/ime.c's keys in each mode, device-free: a double of Rime's session
 * (tools/rime-double/rime_api.h) behaves as librime does with the keys it is
 * given (letters compose; Space confirms the highlighted candidate, Return
 * commits the raw input, digits and keypad digits select, other printable
 * keys commit the highlighted candidate and go on), so what ime.c gives it
 * shows. Both front ends' entry points: ime_process_key (the direct
 * programs, the bar's digits) and ime_key_event (the bar's keysyms).
 *   English: nothing typed is lost or changed without Tab (#291).
 *   Chinese: digits select, as before.
 *   Direct input: every key passes, Rime sees none.
 *   A window (LOCACHY_BACKEND=wayland): no Rime of its own.
 * Build: -DCONFIG_IME -I tools/rime-double. */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int fixture_access(const char *path, int mode);
#define access fixture_access
#include "../base/ime.c"
#undef access

static int fails;
static void check(const char *what, int ok)
{
    printf("%s %s\n", ok ? "PASS" : "FAIL", what);
    fails += !ok;
}

/* Chinese Pinyin installed (rime-frost, no context_pinyin), as
 * installed_cn_schema looks, and Rime deployed: whatever this host has. */
static int fixture_access(const char *path, int mode)
{
    if (strcmp(path, "/usr/share/rime-data/rime_frost.schema.yaml") == 0 || strstr(path, "/build/default.yaml")) {
        return 0;
    }
    if (strncmp(path, "/usr/share/rime-data/", 21) == 0 || strncmp(path, "/var/lib/locachy/rime/", 22) == 0) {
        return -1;
    }
    return access(path, mode);
}

int rc_read(void (*fn)(const char *, const char *, void *), void *arg)
{
    (void)fn;
    (void)arg;
    return 0;
}

static int modifiers; /* keys_modifiers of the direct program */
int keys_modifiers(void)
{
    return modifiers;
}

/* The double: one session, its schema, the raw input, the highlighted
 * candidate and what was committed; the keys Rime was given. */
static struct {
    char schema[32], input[64], commit[128];
    int highlighted, sessions, apis;
    unsigned int keys[256];
    int nkeys;
} rd;

static const struct {
    const char *schema, *input, *candidates[4];
} words[] = {
    /* easy_en with enable_completion: a completion can come first
     * (the acceptance run's "ab " became "abbe") */
    { "easy_en", "ab", { "abbe", "abbey", "ab", NULL } },
    { "easy_en", "cd", { "cdr", "cd", NULL } },
    { "easy_en", "ef", { "eff", "ef", NULL } },
    { "easy_en", "x", { "xx", "xv", "xi", NULL } },
    { "easy_en", "k", { "kw", "k", NULL } },
    { "rime_frost", "ni", { "\xe4\xbd\xa0", "\xe5\xb0\xbc", "\xe6\xb3\xa5", NULL } },
};

static const char *const *candidates(int *n)
{
    *n = 0;
    for (size_t i = 0; i < sizeof words / sizeof words[0]; i++) {
        if (strcmp(words[i].schema, rd.schema) == 0 && strcmp(words[i].input, rd.input) == 0) {
            while (*n < 4 && words[i].candidates[*n]) ++*n;
            return words[i].candidates;
        }
    }
    return NULL;
}

static void rd_commit(const char *text)
{
    snprintf(rd.commit + strlen(rd.commit), sizeof rd.commit - strlen(rd.commit), "%s", text);
    rd.input[0] = '\0';
    rd.highlighted = 0;
}

static void rd_select(int index)
{
    int n;
    const char *const *c = candidates(&n);
    rd_commit(index < n ? c[index] : rd.input);
}

static void rd_setup(RimeTraits *t) { (void)t; }
static void rd_finalize(void) {}
static Bool rd_maintenance(Bool full) { (void)full; return True; }
static void rd_join(void) {}
static RimeSessionId rd_create(void) { rd.sessions++; return 1; }
static Bool rd_destroy(RimeSessionId s) { (void)s; rd.sessions--; return True; }
static void rd_clear(RimeSessionId s) { (void)s; rd.input[0] = '\0'; rd.highlighted = 0; }
static void rd_set_option(RimeSessionId s, const char *o, Bool v) { (void)s; (void)o; (void)v; }
static Bool rd_get_option(RimeSessionId s, const char *o) { (void)s; (void)o; return False; }
static const char *rd_input(RimeSessionId s) { (void)s; return rd.input; }

static Bool rd_schema(RimeSessionId s, const char *id)
{
    (void)s;
    snprintf(rd.schema, sizeof rd.schema, "%s", id);
    rd.input[0] = '\0';
    return True;
}

static Bool rd_get_commit(RimeSessionId s, RimeCommit *c)
{
    (void)s;
    if (!rd.commit[0]) return False;
    c->text = strdup(rd.commit);
    rd.commit[0] = '\0';
    return True;
}

static Bool rd_free_commit(RimeCommit *c)
{
    free(c->text);
    c->text = NULL;
    return True;
}

static RimeCandidate shown[4];
static Bool rd_get_context(RimeSessionId s, RimeContext *ctx)
{
    (void)s;
    int n;
    const char *const *c = candidates(&n);
    memset(&ctx->composition, 0, sizeof ctx->composition);
    memset(&ctx->menu, 0, sizeof ctx->menu);
    ctx->composition.preedit = rd.input[0] ? rd.input : NULL;
    for (int i = 0; i < n; i++) shown[i].text = (char *)c[i];
    ctx->menu.num_candidates = rd.input[0] ? n : 0;
    ctx->menu.candidates = shown;
    ctx->menu.highlighted_candidate_index = rd.highlighted;
    return True;
}

static Bool rd_free_context(RimeContext *ctx) { (void)ctx; return True; }

static Bool rd_select_on_page(RimeSessionId s, size_t index)
{
    (void)s;
    int n;
    candidates(&n);
    if (!rd.input[0] || (int)index >= n) return False;
    rd_select((int)index);
    return True;
}

static Bool rd_process_key(RimeSessionId s, int sym, int mask)
{
    (void)s;
    int release = (mask & (1 << 30)) != 0;
    if (rd.nkeys < 256) rd.keys[rd.nkeys++] = (unsigned int)sym | (release ? 0x80000000u : 0u);
    if (release) return False;
    int n;
    candidates(&n);
    if (((sym >= 'a' && sym <= 'z') || (sym >= 'A' && sym <= 'Z')) && !(mask & (4 | 8 | 64))) {
        size_t len = strlen(rd.input);
        if (len + 1 < sizeof rd.input) rd.input[len] = (char)sym, rd.input[len + 1] = '\0';
        rd.highlighted = 0;
        return True;
    }
    if (!rd.input[0]) return False;
    if (sym == 0xff08) { rd.input[strlen(rd.input) - 1] = '\0'; rd.highlighted = 0; return True; }
    if (sym == 0xff1b) { rd.input[0] = '\0'; return True; }
    if (sym == ' ') { rd_select(rd.highlighted); return True; }     /* selector: confirm */
    if (sym == 0xff0d) { rd_commit(rd.input); return True; }        /* express_editor: raw */
    if (sym >= '0' && sym <= '9') { if ((sym - '0' + 9) % 10 < n) rd_select((sym - '0' + 9) % 10); return True; }
    if (sym >= 0xffb0 && sym <= 0xffb9) { if ((sym - 0xffb0 + 9) % 10 < n) rd_select((sym - 0xffb0 + 9) % 10); return True; }
    if (sym == 0xff09 || sym == 0xff54) { if (rd.highlighted + 1 < n) rd.highlighted++; return True; }
    if (sym == 0xfe20 || sym == 0xff52) { if (rd.highlighted > 0) rd.highlighted--; return True; }
    if (sym > 0x20 && sym < 0x7f) { rd_select(rd.highlighted); return False; } /* commits, the key goes on */
    return False;
}

static RimeApi api = {
    .setup = rd_setup, .initialize = rd_setup, .finalize = rd_finalize,
    .start_maintenance = rd_maintenance, .join_maintenance_thread = rd_join,
    .create_session = rd_create, .destroy_session = rd_destroy, .process_key = rd_process_key,
    .clear_composition = rd_clear, .get_commit = rd_get_commit, .free_commit = rd_free_commit,
    .get_context = rd_get_context, .free_context = rd_free_context, .set_option = rd_set_option,
    .get_option = rd_get_option, .select_schema = rd_schema, .get_input = rd_input,
    .select_candidate_on_current_page = rd_select_on_page,
};

RimeApi *rime_get_api(void)
{
    rd.apis++;
    return &api;
}

/* What the program shows: each key's commit, then the key if it passed. */
static char typed[256];

static void got(const char *s)
{
    size_t used = strlen(typed), n = strlen(s);
    if (used + n < sizeof typed) memcpy(typed + used, s, n + 1);
}

/* A direct program's key (term, edit, view), as it types: the commit
 * first, then the key unless taken. */
static void direct(int key, const char *as)
{
    int taken = ime_process_key(key);
    const char *c = ime_get_commit();
    if (c) got(c);
    if (!taken) got(as);
}

static void direct_text(const char *s)
{
    for (; *s; s++) direct(*s, (char[]){ *s, 0 });
}

/* The bar's key: its press as a keysym (the X11 mask), then the release,
 * as locachy-bar gives them; digits of the main row and of the keypad with
 * Num Lock on take the candidate path (ime_process_key) as bar.c routes
 * them. The client types the key's own text when it passed. */
static void bar(unsigned int sym, unsigned int mask, const char *as)
{
    int digit = sym >= '0' && sym <= '9' ? (int)sym : sym >= 0xffb0 && sym <= 0xffb9 ? (int)(sym - 0xffb0 + '0') : 0;
    int taken = digit && !(mask & ~1u) ? ime_process_key(digit) : ime_key_event(sym, mask);
    const char *c = ime_get_commit();
    if (c) got(c);
    if (!taken) got(as);
    ime_key_event(sym, mask | (1u << 30));
    c = ime_get_commit();
    if (c) got(c);
}

static void bar_text(const char *s)
{
    for (; *s; s++) {
        unsigned int sym = *s == '\n' ? 0xff0d : (unsigned char)*s;
        bar(sym, *s >= 'A' && *s <= 'Z' ? 1u : 0u, (char[]){ *s, 0 });
    }
}

/* Only letters and BackSpace reached Rime since mark (releases aside). */
static int rime_saw_letters_only(int mark)
{
    for (int i = mark; i < rd.nkeys; i++) {
        unsigned int k = rd.keys[i];
        if (k & 0x80000000u) return 0;
        if (!((k >= 'a' && k <= 'z') || (k >= 'A' && k <= 'Z') || k == 0xff08)) return 0;
    }
    return 1;
}

static const char probe[] = "ab cd-ef 12 x3 qq\n";

int main(void)
{
    unsetenv("LOCACHY_BACKEND");
    if (ime_init(NULL, "/nonexistent") != 0) {
        puts("FAIL ime_init with the Rime double");
        return 1;
    }
    char row1[512], row2[512];

    /* English: the acceptance run's probe, exact, both entry points. */
    ime_set_mode(IME_MODE_EN);
    typed[0] = '\0';
    int mark = rd.nkeys;
    direct_text(probe);
    direct('k', "k");
    direct('1', "1"); /* a keypad digit with Num Lock on comes as '1' */
    direct(' ', " ");
    direct('\n', "\n");
    check("EN direct: \"ab cd-ef 12 x3 qq\\n k1 \\n\" typed exactly (Space, -, digits, Return pass; nothing chosen)",
          strcmp(typed, "ab cd-ef 12 x3 qq\nk1 \n") == 0);
    if (strcmp(typed, "ab cd-ef 12 x3 qq\nk1 \n") != 0) printf("     typed: \"%s\"\n", typed);
    check("EN direct: Rime was given letters only", rime_saw_letters_only(mark));

    typed[0] = '\0';
    mark = rd.nkeys;
    bar_text(probe);
    bar('k', 0, "k");
    bar(0xffb1, 0, "1"); /* KP_1 with Num Lock on */
    bar(0xffb2, 0, "2");
    bar(' ', 0, " ");
    bar(0xffb3, 0, "3");
    bar(0xff8d, 0, "\n"); /* KP_Enter */
    check("EN bar: the probe and keypad digits typed exactly (\"...k12 3\\n\")",
          strcmp(typed, "ab cd-ef 12 x3 qq\nk12 3\n") == 0);
    if (strcmp(typed, "ab cd-ef 12 x3 qq\nk12 3\n") != 0) printf("     typed: \"%s\"\n", typed);
    check("EN bar: Rime was given letters only, no release", rime_saw_letters_only(mark));

    /* The overlay: completions without digit labels, none chosen. */
    typed[0] = '\0';
    direct('a', "a");
    direct('b', "b");
    int shown_now = ime_get_overlay(row1, row2, sizeof row1);
    check("EN overlay: the word and its completions, no digit labels, none chosen",
          shown_now && strstr(row1, "ab") && strstr(row1, "abbe") && !strstr(row1, "1.") && !strchr(row1 + 1, '['));
    /* Tab moves to a completion, Space commits it and is typed. */
    direct('\t', "\t");
    ime_get_overlay(row1, row2, sizeof row1);
    check("EN overlay: Tab brackets the first completion", strstr(row1, "[abbe]") != NULL);
    direct(' ', " ");
    check("EN Tab, Space: the completion, then the space", strcmp(typed, "abbe ") == 0);
    /* Tab twice, Return: the second, then the newline. */
    typed[0] = '\0';
    direct_text("ab");
    direct('\t', "\t");
    direct('\t', "\t");
    direct('\n', "\n");
    check("EN Tab Tab Return: the second completion, then the newline", strcmp(typed, "abbey\n") == 0);
    /* Past the last: none again, the word as typed. */
    typed[0] = '\0';
    direct_text("ab");
    for (int i = 0; i < 4; i++) direct('\t', "\t");
    direct('.', ".");
    check("EN Tab past the last completion: the letters as typed again", strcmp(typed, "ab.") == 0);
    /* Shift+Tab: from none to the last (the bar's ISO_Left_Tab, a direct
     * program's Tab with Shift). */
    typed[0] = '\0';
    bar('a', 0, "a");
    bar('b', 0, "b");
    bar(0xfe20, 1, "");
    bar(0xff0d, 0, "\n");
    modifiers = KEYS_SHIFT;
    direct('x', "x");
    direct('\t', "\t");
    modifiers = 0;
    direct(' ', " ");
    check("EN Shift+Tab: from none to the last completion (bar and direct)", strcmp(typed, "ab\nxi ") == 0);
    /* A letter after Tab: the word as typed again. */
    typed[0] = '\0';
    direct_text("ab");
    direct('\t', "\t");
    direct('c', "c");
    direct(' ', " ");
    check("EN a letter after Tab: the word as typed", strcmp(typed, "abc ") == 0);
    /* BackSpace edits the word; with none, it passes. */
    typed[0] = '\0';
    direct_text("abc");
    direct('\b', "<BS>");
    direct(' ', " ");
    direct('\b', "<BS>");
    check("EN BackSpace: takes a letter back, passes with no word", strcmp(typed, "ab <BS>") == 0);
    /* Tab with no completion passes on, as typed. */
    typed[0] = '\0';
    direct_text("qq");
    direct('\t', "\t");
    check("EN Tab without completions: the word, then the Tab", strcmp(typed, "qq\t") == 0);
    /* Escape, arrows, Ctrl+C, a character beyond ASCII: the word, then the key. */
    typed[0] = '\0';
    direct_text("ab");
    direct(27, "<Esc>");
    direct_text("cd");
    direct(K_LEFT, "<Left>");
    direct_text("ef");
    direct(3, "^C");
    direct_text("x");
    direct(K_TEXT, "\xc3\xa4");
    bar('k', 0, "k");
    bar('c', 4, "^C"); /* Ctrl+c through the bar */
    bar('q', 0, "q");
    bar(0xff1b, 0, "<Esc>");
    check("EN Escape, arrows, Ctrl, beyond ASCII: the word as typed, then the key",
          strcmp(typed, "ab<Esc>cd<Left>ef^Cx\xc3\xa4k^Cq<Esc>") == 0);
    if (strcmp(typed, "ab<Esc>cd<Left>ef^Cx\xc3\xa4k^Cq<Esc>") != 0) printf("     typed: \"%s\"\n", typed);
    /* Modifiers alone change nothing. */
    typed[0] = '\0';
    bar('a', 0, "a");
    bar(0xffe1, 1, ""); /* Shift_L */
    bar('B', 1, "B");
    bar(' ', 0, " ");
    check("EN Shift alone keeps the word: \"aB \"", strcmp(typed, "aB ") == 0);
    /* Switching the mode or locking Caps commits the word as typed. */
    direct_text("ab");
    const char *notice = NULL;
    ime_action(K_CTRL_SPACE, &notice); /* EN -> RAW */
    const char *c = ime_get_commit();
    check("EN switched away: the word committed as typed", ime_mode() == IME_MODE_RAW && c && strcmp(c, "ab") == 0);
    ime_set_mode(IME_MODE_EN);
    direct_text("cd");
    ime_caps_key(1, 1);
    ime_set_caps(1);
    c = ime_get_commit();
    int caps_passes = ime_process_key('E') == 0;
    ime_set_caps(0);
    check("EN Caps Lock: the word committed as typed, capitals pass", c && strcmp(c, "cd") == 0 && caps_passes);

    /* Chinese: digits select, as before. */
    ime_set_mode(IME_MODE_CN);
    typed[0] = '\0';
    direct_text("ni");
    direct('1', "1");
    direct_text("ni");
    bar('2', 0, "2");
    direct_text("ni");
    bar(0xffb3, 0, "3"); /* keypad digit, Num Lock on */
    direct('5', "5");    /* no pinyin: the digit */
    check("CN digits (main row, keypad) select; without pinyin typed",
          strcmp(typed, "\xe4\xbd\xa0\xe5\xb0\xbc\xe6\xb3\xa5" "5") == 0);
    if (strcmp(typed, "\xe4\xbd\xa0\xe5\xb0\xbc\xe6\xb3\xa5" "5") != 0) printf("     typed: \"%s\"\n", typed);
    typed[0] = '\0';
    direct_text("ni");
    direct(' ', " ");
    check("CN Space confirms the highlighted candidate (Rime's)", strcmp(typed, "\xe4\xbd\xa0") == 0);
    direct_text("ni");
    ime_set_caps(1);
    c = ime_get_commit();
    ime_set_caps(0);
    check("CN Caps Lock drops the pinyin (as before)", c == NULL && !ime_get_overlay(row1, row2, sizeof row1));

    /* Direct input: every key passes, Rime is given none. */
    ime_set_mode(IME_MODE_RAW);
    typed[0] = '\0';
    mark = rd.nkeys;
    direct_text(probe);
    bar_text(probe);
    bar(0xffb1, 0, "1");
    check("RAW: every key passes as typed, Rime given none",
          strcmp(typed, "ab cd-ef 12 x3 qq\nab cd-ef 12 x3 qq\n1") == 0 && rd.nkeys == mark);
    ime_done();

    /* A window on the shared display: the bar's input method is the
     * window's; no Rime of its own (its user data is the bar's). */
    setenv("LOCACHY_BACKEND", "wayland", 1);
    int apis = rd.apis, sessions = rd.sessions;
    int status = ime_init(NULL, "/nonexistent");
    ime_action(K_CTRL_SPACE, &notice);
    int mode = ime_mode();
    typed[0] = '\0';
    direct_text("ab ");
    check("window: no Rime started, direct input kept, keys pass",
          status == 0 && rd.apis == apis && rd.sessions == sessions && mode == IME_MODE_RAW &&
          strcmp(typed, "ab ") == 0 && !ime_get_overlay(row1, row2, sizeof row1));
    ime_done();
    unsetenv("LOCACHY_BACKEND");
    check("direct program: its own Rime", ime_init(NULL, "/nonexistent") == 0 && rd.apis == apis + 1 && rd.sessions == 1);
    ime_done();

    puts(fails ? "FAIL ime keys" : "PASS ime keys: English never loses or changes typing; Chinese digits select; "
                                   "direct input passes; a window has no Rime of its own");
    return fails != 0;
}
