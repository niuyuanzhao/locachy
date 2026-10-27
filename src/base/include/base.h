/*
 * The base library on the Linux kernel: what every payload links. The
 * kernel is only the hardware layer; a payload reaches it through its stable
 * interface alone (system calls and device files), so the kernel can be
 * replaced by a newer one without touching any of this.
 *
 * Programs see only this header and are linked statically (musl). The
 * library and every program are built together from the same source.
 */
#ifndef BASE_H
#define BASE_H

#include <stddef.h>
#include <stdint.h>

/* Log a line to the console (the serial port in QEMU): printf format, no
 * newline needed. */
void say(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

/* Seconds since the kernel started. */
double uptime(void);

/* Format system status (Wi-Fi, Battery/AC, optional IME mode tag, and clock). */
void base_status(const char *ime_tag, char *out, size_t size);

/* The status as short texts for a bar with two rows (locachy-bar), with also
 * the microphone, memory and CPU; "" for what is not there. */
struct status_fields {
    char volume[24], mic[24];          /* "Vol 50%"         "Mic 100%" */
    char bluetooth[24], bt_device[48]; /* "BT on"           "idle" / device */
    char network[80], address[64];     /* "Wi-Fi HomeNet"   "192.168.1.23" */
    char battery[24], power[40];       /* "Bat 80%"         "7.5 W, 3h20m" */
    char memory[32], swap[32];         /* "RAM 3.1/15.4G"   "Swap 0.0/20.0G" */
    char cpu[16], temp[16];            /* "CPU 12%"         "52°C" */
    char time[16], date[24];           /* "03:18:51"        "Tue 2026-10-20" */
};
void base_status_fields(struct status_fields *out);
#define MACHINES_DIR "/usr/local/share/locachy/machines"
int base_brightness_get(int *cur, int *max_val);
int base_brightness_set(int target, int max_ref);
int base_brightness_restore(void);
int base_brightness_key(int action_key, char *toast, size_t size);

/* The system's startup file (rc.c): NAME="VALUE" lines as in BSD's rc.conf.
 * rc_read calls fn for each setting, in file order; -1 if there is no file. */
#define RC_CONF "/etc/locachy/rc.conf"
int rc_read(void (*fn)(const char *name, const char *value, void *arg), void *arg);
/* NAME="VALUE" in place of the line(s) of NAME, or at the end; value NULL:
 * none. VALUE has no quote or newline. 0, or -1. */
int rc_set(const char *name, const char *value);
/* The same for another file of that form (a container's configuration). */
int conf_read(const char *path, void (*fn)(const char *name, const char *value, void *arg), void *arg);
int conf_set(const char *path, const char *name, const char *value);

/* The interface's language (i18n.c): GNU gettext, domain
 * "locachy", catalogs LOCALE_DIR/LL_CC/LC_MESSAGES/locachy.mo compiled from
 * po/LL_CC.po by make.sh. rc.conf's language="zh_CN.UTF-8" selects the
 * generated locale for LC_MESSAGES; none, English or a locale that is not
 * there: the English the program is written in. Only interface text is
 * marked, where it is written: _("...") translates it there; N_("...") marks
 * text of a static table, translated where it is shown (_(table[i]));
 * C_("context", "...") for a short text that means two things; P_("one",
 * "many", n) for a count. Never file, package or container names, types,
 * logs or configuration keys. base_language_reload: rc.conf read again
 * (settings changes it, then SIGHUP). LOCACHY_LANG and LOCACHY_LOCALE_DIR
 * override both, for tests. */
#define LOCALE_DIR "/usr/local/share/locale"
#define I18N_DOMAIN "locachy"
const char *base_tr(const char *msgid);
const char *base_trc(const char *context, const char *msgid);
const char *base_trn(const char *one, const char *many, unsigned long n);
#define _(s) base_tr(s)
#define N_(s) (s)
#define C_(c, s) base_trc(c, s)
#define P_(one, many, n) base_trn(one, many, n)
const char *base_language(void); /* rc.conf's language, "" when English */
void base_language_reload(void);

/* The shared compositor's configuration as used: /etc/locachy/labwc/rc.xml
 * with rc.conf's values, written by settings. */
#define LABWC_RC "/run/locachy/labwc-rc.xml"

/* The screens' settings, written by settings: lines CONNECTOR=PLACE,MODE,SCALE
 * (PLACE origin, right, left, above or below; MODE WIDTHxHEIGHT or preferred;
 * SCALE fractional). The same scale applies to the shared display and to the
 * framebuffer programs. monitor_conf: 1 and *out if connector name has a line;
 * otherwise 0 and the defaults (right, preferred, scale 0: not set). */
#define MONITORS_CONF "/etc/locachy/monitors.conf"
struct monitor_conf {
    char place[16];
    int width, height; /* 0: preferred */
    double scale;      /* 0: not set */
};
int monitor_conf(const char *name, struct monitor_conf *out);
/* Whether connector name is the computer's own screen: an eDP, LVDS or DSI
 * one, or, when rc.conf says internal_screen="NAME", that one only (a panel
 * on another kind of connector, as the X230's on DP-3). */
int monitor_internal(const char *name);

/* The screen (screen.c): one logical picture mirrored to connected outputs
 * of the selected DRM card, each in its preferred mode. Other output sizes
 * fit the fixed logical picture with black borders. Text is Terminus from
 * BASE_FONT_DIR, TEXT_H pixels high at the first output's scale of 1 (its
 * MONITORS_CONF scale, else 1; 14 to 56 pixels, above 32 a Terminus of half
 * the size drawn twice as large), and the top bar one and a half rows;
 * font_path is the font when those cannot be read. Colours are 0xRRGGBB;
 * positions are character cells. Returns 1 if any output is usable; otherwise
 * logs why. BASE_FONT is Terminus 16 bold (the initramfs has only that). */
#define BASE_FONT "/usr/local/share/locachy/font.psf"
int screen_open(const char *font_path);
/* Read the scale again (settings changed it, or another output became the
 * first): 1 if the font and so the cells changed, and the picture is cleared
 * for a redraw. Taking the screen back (K_SHOWN) does this by itself. */
int screen_rescale(void);
int screen_cols(void);
int screen_rows(void);
void screen_describe(void); /* one log line: resolution, scale, font, cells */
void screen_pixels(int *width, int *height);
void screen_cell_at(int x, int y, int *col, int *row);
void screen_fill(int col, int row, int cols, int rows, uint32_t rgb);
/* Text from (col, row), clipped at the end of the line. */
void screen_text(int col, int row, const char *s, uint32_t fg, uint32_t bg);
/* The cells screen_text gives character cp: 2 for a wide (CJK) one, else 1. */
int screen_char_cells(uint32_t cp);
/* Pictures (a document's page): rows of 0xRRGGBB pixels, stride pixels a
 * row, drawn at pixel (x, y) of the picture and clipped to it; and a filled
 * rectangle of pixels. */
void screen_blit(int x, int y, int w, int h, const uint32_t *px, int stride);
void screen_fill_px(int x, int y, int w, int h, uint32_t rgb);
/* The cells in pixels: each is *w by *h, and row 0 starts *top pixels down. */
void screen_cells(int *w, int *h, int *top);
/* Make what was drawn since the last flush visible. */
void screen_flush(void);
/* The status bar at the top, as on the shared display (panel.c), with title
 * (the program) and ime (its input method mode, or NULL). The cells start
 * half a row below the top, so that row 0 is within the bar:
 * draw it after anything that covers row 0. */
void screen_bar(const char *title, const char *ime);

/* The framebuffer programs' text row at scale 1, and the shared display's
 * overlays' (locachy-bar): 18, a size larger than the 16 of a console, as
 * Terminus is narrow and light beside the shared display's programs' fonts
 * (Edge's tab titles at 150%: capitals 16 pixels high, Terminus 24's 15, and
 * wider and heavier); there, 125% gives Terminus 24. */
#define TEXT_H 18
/* The text row in pixels at a scale: TEXT_H times it, the nearest Terminus
 * size (above 32, one of half the size drawn twice as large). The direct
 * screens' cells, and at each screen's scale the shared display's bar (one
 * and a half of these rows) and overlays: the same on both sides. */
int text_px(double scale);
/* The status bar (panel.c), the same on the shared display (locachy-bar,
 * at each screen's scale) and at the top of the framebuffer programs: two
 * rows of text, PANEL_H pixels at scale 1: one and a half rows of the text
 * below it. Drawing is into ARGB pixels px, w wide and h high (the bar's
 * height there), with stride pixels a row. */
#define PANEL_H (TEXT_H * 3 / 2)
#define PANEL_BG 0xFF203048u
#define BASE_FONT_DIR "/usr/local/share/locachy/terminus" /* ter-uNN[nb].psf, 12..32 pixels */
/* LOCachy, the title block (title over sub), memory and CPU at the left; the
 * input method (if ime), volume, network, battery and clock at the right; the
 * pixels between them are [*mid_x0, *mid_x1), at least reserve wide if the
 * left blocks can make room (CPU goes first, then memory). */
void panel_draw(uint32_t *px, int w, int h, int stride, const char *title, const char *sub,
                const char *ime, int reserve, int *mid_x0, int *mid_x1);
/* The largest Terminus at most height pixels high (bold or not); NULL if
 * none can be read. */
struct panel_font;
const struct panel_font *panel_font(int height, int bold);
void panel_font_size(const struct panel_font *f, int *w, int *h);
/* Text from pixel (x, y), clipped at w and h. */
void panel_text(uint32_t *px, int w, int h, int stride, int x, int y, const char *s,
                uint32_t fg, uint32_t bg, const struct panel_font *f);
int panel_text_width(const char *s, const struct panel_font *f);
/* s cut to fit width pixels, ending in an ellipsis when it was cut. */
void panel_text_fit(char *out, size_t size, const char *s, int width, const struct panel_font *f);
/* The bytes of the longest start of s that fits in width pixels. */
size_t panel_text_cut(const char *s, int width, const struct panel_font *f);
void panel_fill(uint32_t *px, int w, int h, int stride, int x, int y, int bw, int bh, uint32_t argb);
/* How many containers are frozen; *ram: the memory of container name. */
int panel_containers(const char *name, unsigned long long *ram);

/* The keyboard (keys.c): every evdev device with letter keys, typing as the
 * layout's key table has it (keytable, from rc.conf's keymap; US without
 * it). A key is a character (Ctrl with a letter gives the control
 * character), or one of these. */
#define K_UP    0x100
#define K_DOWN  0x101
#define K_LEFT  0x102
#define K_RIGHT 0x103
#define K_PGUP  0x104
#define K_PGDN  0x105
#define K_BOL   0x106 /* Home */
#define K_EOL   0x107 /* End */
#define K_DEL   0x108 /* Delete */
#define K_CTRL_SPACE 0x109 /* IME toggle */
#define K_INS   0x10A /* Insert */
#define K_F1    0x120 /* K_F1 + (0..11) for F1..F12 */
#define K_CTRL_0     0x110 /* K_CTRL_0 + (0..9) for Ctrl+0..Ctrl+9 */
#define K_VOL_UP     0x1F0
#define K_VOL_DOWN   0x1F1
#define K_VOL_MUTE   0x1F2
#define K_MIC_MUTE   0x1F3
#define K_BRI_UP     0x1F4
#define K_BRI_DOWN   0x1F5
#define K_ACT_IME_CN    0x1F6 /* key_ime_cn: the input method in Chinese mode */
#define K_ACT_IME_RAW   0x1F7 /* key_ime_raw: direct input */
#define K_ACT_IME_WIDTH 0x1F8 /* key_ime_width: Chinese mode's punctuation, full or half width */
#define K_NAMED      0x1F9 /* while keys_name_any: a key that types nothing, bound to nothing */
#define K_TEXT       0x1FA /* a key typed text that is not one ASCII character (ä, €, a dead key's é): keys_text */
#define K_ACT_LOCK   0x1FB /* key_lock: process 1 locks the screen */
#define K_ACT_HOME   0x1FE
/* Not a key: the screen and the keyboard have just been given to this
 * program (first, and again each time process 1 gives them back after a
 * switch). The picture is shown again as it was; check what needs it. */
#define K_SHOWN 0x1FF
int keys_open(void);    /* the number of keyboards found */
int key_get(void);      /* wait for the next key */
int key_poll(int fd);   /* like key_get, or 0 when fd is ready to read */
int key_poll_timeout(int fd, int timeout_ms); /* with millisecond timeout */
int keys_match_action(int ev_code, int ctrl, int alt, int shift, int meta);
void keys_reload(void); /* rc.conf's keys and the key table read again at the next key */
/* The key table at path instead of the system's (the installer's, for the
 * layout chosen); -1 when it has none (US). */
int keys_layout_load(const char *path);
/* The key table's layout: "console" (rc.conf's keymap), and the X names
 * "layout", "model", "variant", "options"; NULL when not given. */
const char *keys_layout_name(const char *field);
int keys_char(int code, int shift); /* what an evdev key types, ASCII; 0 for nothing */
/* K_TEXT's text, UTF-8. Weak: programs tested without keys.c type ASCII. */
__attribute__((weak)) const char *keys_text(void);
/* What key typed into a line of text, UTF-8: a printable ASCII character,
 * or K_TEXT's text; NULL for any other key. Valid until the next call. */
static inline const char *keys_typed(int key)
{
    static char one[2];
    if (key >= 0x20 && key < 0x7F) {
        one[0] = (char)key;
        return one;
    }
    return key == K_TEXT && keys_text ? keys_text() : NULL;
}
/* The last key pressed and the modifiers held, as rc.conf names keys
 * (Ctrl+Alt+T): a letter, digit, Space, Enter or Tab, or a key that types
 * nothing by its keysym name (Henkan_Mode); -1 for others. */
int keys_last_name(char *out, size_t size);
/* While on, a key that types nothing and is bound to nothing (Hiragana_Katakana)
 * comes as K_NAMED, for keys_last_name, instead of not at all: settings
 * records keys with it. */
void keys_name_any(int on);
/* The modifiers held at the last key (for Ctrl+. and Shift+Space, which
 * come as '.' and ' '). */
#define KEYS_SHIFT 1
#define KEYS_CTRL  2
#define KEYS_ALT   4
#define KEYS_META  8
int keys_modifiers(void);
/* Poll extra file descriptors while retaining keyboard and process-1 control
 * handling. Extra descriptors keep their events and receive revents. */
struct pollfd;
int key_poll_many(struct pollfd *extra, size_t count, int timeout_ms);

/* The pointer (pointer.c): mice, touchpads, the TrackPoint, touch screens
 * and pens through libinput, set up as on the shared display (rc.conf's
 * pointer_accel, tap to click, two-finger and TrackPoint scrolling). A
 * touch screen's tap is a click, holding it a right click, dragging scrolls
 * (wheel steps); pinches come as GESTURE_PINCH. Process 1 lends the devices
 * to the program on the screen; keys_open asks for them. Connected devices
 * update in place, without a screen activation or restarting the program.
 * key_get and key_poll* return K_POINTER for each event (motions merged),
 * described by pointer_last(). The screen draws the cursor where it is. */
#define K_POINTER 0x1FD
struct pointer_event {
    int x, y;      /* pixels of the picture */
    int col, row;  /* its cell; row -1: the top bar */
    int button;    /* BTN_LEFT, BTN_RIGHT or BTN_MIDDLE pressed or released; 0: a motion or scroll */
    int pressed;   /* 1 pressed, 0 released */
    int clicks;    /* a press: 1, 2 for a double click, 3 for a triple one */
    int wheel;     /* scroll steps, positive up */
    int hwheel;    /* positive right */
    int buttons;   /* held: 1 left, 2 right, 4 middle */
    int gesture;   /* GESTURE_PINCH: two fingers apart or together, by scale */
    double scale;  /* a pinch: the factor since its last event (above 1: apart, zoom in) */
};
enum { GESTURE_NONE, GESTURE_PINCH };
const struct pointer_event *pointer_last(void);
int pointer_present(void); /* a pointer device is there */
/* The cursor's shape: an arrow, a text beam (where text is selected), none. */
enum { CURSOR_ARROW, CURSOR_TEXT, CURSOR_NONE };
void screen_cursor(int shape);

/* Clickable places (ui.c): a program adds them as it draws (cells), and
 * looks up which one a pointer event is on: its id, or -1. ui_button draws
 * [label] there as one and returns the column after it. ui_menu shows items
 * (up to a NULL) at (col, row) over the picture, chosen with the keys or the
 * pointer: the index, or -1 (Esc, a click elsewhere); then redraw. */
void ui_hits_clear(void);
void ui_hit(int col, int row, int ncols, int nrows, int id);
int ui_hit_at(int col, int row);
int ui_button(int col, int row, const char *label, int id);
/* Display cells of UTF-8 text (CJK: two), and s padded with spaces to cells:
 * for columns of text in any language (%-*s counts bytes). */
int ui_cells(const char *s);
const char *ui_pad(char *out, size_t size, const char *s, int cells);
int ui_menu(int col, int row, const char *const *items);

int base_audio_key(int action_key, char *toast, size_t size);

/*
 * Calls (control.c). One program runs at a time; the others are frozen with
 * their memory pushed out to swap, and continue where they were when they
 * run again: nothing is restarted.
 *
 * base_call runs another program and waits for it to return, like a
 * function: process 1 takes the screen and keyboard from this program,
 * freezes it, starts argv[0] (a full path) on top of it, and
 * when that exits, thaws this program, gives them back and base_call
 * returns its exit status (-1 if it could not be asked, 127 if it would not
 * start). Ctrl+Alt+Esc meanwhile goes to home with the whole stack frozen.
 */
int base_call(char *const argv[]);

/* For home: show the task argv (start it if it is not running) and wait
 * until home has the screen again; why says what happened: "back NAME"
 * (Ctrl+Alt+Esc) or "ended NAME STATUS" (its last program exited). */
void base_open(char *const argv[], char *why, size_t size);

/* For home: the tasks there are, one line each: "NAME: prog > prog ...",
 * NAME being the argv that opened it, joined with spaces. */
void base_tasks(char *buf, size_t size);

/* For home: ask process 1 to terminate and clean up the task argv. */
void base_kill(char *const argv[]);

/* Host container controls. Rows: name, state, RAM bytes, swap bytes, error,
 * separated by tabs. Actions are accepted asynchronously; poll for completion. */
void base_apps(char *buf, size_t size);
int base_app_action(const char *action, const char *name, char *why, size_t size);
/* For home: open target (a path or URL) in container name, starting it
 * (and the shared display) if it is not running. */
int base_app_open(const char *name, const char *target, char *why, size_t size);

/* Containers: each is its configuration, CONTAINERS_CONF/NAME.conf, lines
 * NAME="VALUE" as in rc.conf: command (what it runs), network ("1": in
 * locachy-net), packages (its own, over the host's). Their state (overlay,
 * home) is under /var/lib/locachy/containers/NAME. */
#define CONTAINERS_CONF "/etc/locachy/containers"

/* Opening files and addresses (open.c): which container opens what. A
 * target is an absolute path or a URL; its type is a MIME type (named as
 * shared-mime-info names it: file(1)'s answer, made exact by the file's name
 * where that is a kind of it), inode/directory, or x-scheme-handler/SCHEME.
 * A program that opens a type opens its subtypes too (text/plain: C source). */
#define OPEN_RULES "/etc/locachy/open.conf"
#define FILES_REQUEST "/run/locachy/files-request" /* a folder for home's file manager */
#define OPEN_NAME 64 /* container names, with the NUL */
#define OPEN_TYPE 128 /* types, with the NUL */
enum { OPEN_NO_RULE, OPEN_RULE_CONTAINER, OPEN_RULE_ASK };
int open_normalize(const char *target, char *out, size_t size); /* file:// URL -> path */
int open_type(const char *target, char *type, size_t size);     /* 0, or -1: cannot tell */
int open_shared(const char *path, char *why, size_t size);      /* 1: in a shared folder */
int open_host(const char *name);                                 /* 1: a host program (host="1") */
void open_user_dirs(char *home, size_t hsize, char *dirs, size_t dsize);
int open_candidates(const char *type, char names[][OPEN_NAME], int max);
int open_containers(char names[][OPEN_NAME], int max);
/* The app ids of container name's windows: its .desktop files' names and
 * StartupWMClass values, as desktop shells match windows to programs. */
#define OPEN_WINDOW_ID 128
int open_window_ids(const char *name, char ids[][OPEN_WINDOW_ID], int max);
void open_label(const char *name, const char *type, char *label, size_t size);
int open_rule(const char *type, char *name, size_t size);       /* OPEN_NO_RULE...; name if CONTAINER */
int open_set_rule(const char *type, const char *value);         /* a container, "ask", or NULL */
int open_rules(char types[][OPEN_TYPE], char values[][OPEN_NAME], int max); /* all, in file order */
/* The framebuffer programs (home's, rc.conf program_NAME) open types too:
 * as rule values and candidates they are "program:NAME", their types the
 * MimeType of OPEN_PROGRAM_DESKTOP/NAME.desktop. They see the user's files
 * as the user does, not only the shared folders. */
#define OPEN_PROGRAM "program:"
#define OPEN_PROGRAM_DESKTOP "/usr/local/share/applications/locachy"
/* The command of program name ("program:NAME") with target after it, split
 * in spec into argv (at most max entries with its NULL): the number of
 * words, or 0 if name is not one. */
int open_program(const char *name, const char *target, char *spec, size_t size, char *argv[], int max);
/* Home opens target as its file manager does (the rule for its type, or the
 * user's choice), or with program with, coming to the front for it: for a
 * program on the screen (view's links), which this asks process 1 for home,
 * and locachy-bar (with a program a container's target is for). 0, or -1. */
#define OPEN_REQUEST "/run/locachy/open-request"
int open_in_home(const char *with, const char *target);

/* Ask process 1 to reboot or power off. */
void base_reboot(void);
void base_poweroff(void);

#endif
