/* Pure terminal grid and nonblocking PTY-output tests: no display or input
 * device is opened, no real clipboard is accessed; pointer events are
 * synthetic. */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#define main term_main
#include "../progs/term.c"
#include "i18n-stub.h"
#undef main
#include <assert.h>

static int mods;
static char copied[256];
int keys_modifiers(void) { return mods; }
int clipboard_set(int primary, const void *text, size_t size)
{
    assert(primary && size < sizeof copied);
    memcpy(copied, text, size);
    copied[size] = '\0';
    return CLIPBOARD_OK;
}
const char *clipboard_error(int status) { (void)status; abort(); }
int clipboard_get(int primary, int *fd, size_t *size) { (void)primary; (void)fd; (void)size; abort(); }
double uptime(void) { abort(); }

static void setup_grid(int width, int height)
{
    free(grid); free(alt_grid); free(dirty); free(soft); free(alt_soft);
    free(history); free(history_soft);
    history = NULL; history_soft = NULL;
    hist_len = hist_head = view = 0;
    grid = calloc((size_t)width * height, sizeof *grid);
    alt_grid = calloc((size_t)width * height, sizeof *grid);
    dirty = calloc((size_t)height, 1);
    soft = calloc((size_t)height, 1);
    alt_soft = calloc((size_t)height, 1);
    cols = width; rows = height;
    reset_term();
    state = ST_NORM;
    utf_need = 0;
}

static void feed_text(const char *s)
{
    while (*s) feed((uint8_t)*s++, -1);
}

/* A pointer event on cell (col, row) of the terminal (the bar is above). */
static void point(int col, int row, int button, int pressed, int clicks, int wheel)
{
    struct pointer_event e = { .col = col, .row = row + 1, .button = button, .pressed = pressed,
                               .clicks = clicks, .wheel = wheel };
    pointer_input(&e);
}

static void output_is(const char *expected)
{
    assert(output.size == strlen(expected) && memcmp(output.data, expected, output.size) == 0);
    free(output.data);
    memset(&output, 0, sizeof output);
}

static void choose_text(int a, int b, const char *expected)
{
    selection.active = 1;
    selection.anchor = a;
    selection.end = b;
    size_t size;
    char *text = selection_text(&size);
    assert(text && size == strlen(expected) && strcmp(text, expected) == 0);
    free(text);
}

int main(void)
{
    setup_grid(8, 4);
    feed_text("abcdefghij\r\n中文🌍");
    assert(soft[0] == 1 && soft[1] == 0);
    choose_text(0, 2 * cols + 5, "abcdefghij\n中文🌍");
    choose_text(2 * cols + 1, 2 * cols + 5, "中文🌍");
    choose_text(2 * cols + 5, 2 * cols, "中文🌍");
    puts("PASS selection: soft wraps, hard newlines, wide placeholders and four-byte UTF-8");

    feed_text("\033[?2004h");
    assert(bracketed_paste);
    const char text[] = "one\r\n中文\t🌍\n\033[201~\001\xc2\x9b";
    assert(queue_paste(text, sizeof text - 1) == 0);
    const char expected[] = "\033[200~one\n中文\t🌍\n[201~\033[201~";
    assert(output.size == sizeof expected - 1 && memcmp(output.data, expected, output.size) == 0);
    free(output.data); memset(&output, 0, sizeof output);
    feed_text("\033[?2004l");
    assert(!bracketed_paste && queue_paste("one\ntwo\r\n", 9) == 0);
    assert(output.size == 8 && memcmp(output.data, "one\rtwo\r", 8) == 0);
    free(output.data); memset(&output, 0, sizeof output);
    puts("PASS bracketed paste modes, line endings and control-byte filtering");

    send_key(-1, 3);
    assert(output.size == 1 && output.data[0] == 3);
    free(output.data); memset(&output, 0, sizeof output);
    puts("PASS Ctrl+C remains an interrupt byte");

    static const char *const fkeys[] = { "\033OP", "\033OQ", "\033OR", "\033OS", "\033[15~", "\033[17~",
                                         "\033[18~", "\033[19~", "\033[20~", "\033[21~", "\033[23~", "\033[24~" };
    for (int i = 0; i < 12; i++) {
        send_key(-1, K_F1 + i);
        assert(output.size == strlen(fkeys[i]) && memcmp(output.data, fkeys[i], output.size) == 0);
        free(output.data); memset(&output, 0, sizeof output);
    }
    send_key(-1, K_INS);
    assert(output.size == 4 && memcmp(output.data, "\033[2~", 4) == 0);
    free(output.data); memset(&output, 0, sizeof output);
    puts("PASS F1..F12 and Insert send xterm's sequences");

    int p[2];
    assert(pipe2(p, O_NONBLOCK | O_CLOEXEC) == 0);
    size_t n = 1024 * 1024;
    char *payload = malloc(n);
    assert(payload);
    for (size_t i = 0; i < n; i++) payload[i] = (char)(i % 251);
    assert(queue_output(payload, n) == 0);
    assert(flush_output(p[1]) == 0 && output.offset > 0 && output.offset < output.size);
    size_t received = 0;
    char buf[8192];
    while (received < n) {
        ssize_t got = read(p[0], buf, sizeof buf);
        if (got > 0) {
            assert(memcmp(payload + received, buf, (size_t)got) == 0);
            received += (size_t)got;
        } else assert(errno == EAGAIN);
        assert(flush_output(p[1]) == 0);
    }
    assert(output.size == 0);
    close(p[0]); close(p[1]); free(payload);
    puts("PASS short writes and EAGAIN preserve the complete queued output");
    setup_grid(4, 3);
    feed_text("abc");
    selection.active = 1;
    selection.anchor = 0; selection.end = 2;
    feed_text("d");
    assert(!selection.active);
    puts("PASS overwritten grid content invalidates its selection");

    setup_grid(4, 3);
    feed_text("1\r\n2\r\n3\r\n4\r\n5");
    assert(hist_len == 2 && view == 0 && shown_row(0)[0].ch == '3');
    point(0, 0, 0, 0, 0, 1);
    assert(view == 2 && shown_row(0)[0].ch == '1' && shown_row(2)[0].ch == '3');
    choose_text(0, 2 * cols, "1\n2\n3");
    point(0, 0, 0, 0, 0, -1);
    assert(view == 0);
    view = 1;
    feed_text("\r\n6");
    assert(view == 2 && shown_row(0)[0].ch == '2' && shown_row(2)[0].ch == '4');
    send_key(-1, 'x');
    assert(view == 0);
    output_is("x");
    mods = KEYS_SHIFT;
    send_key(-1, K_PGUP);
    assert(view == 2 && output.size == 0);
    send_key(-1, K_PGDN);
    assert(view == 0 && output.size == 0);
    mods = 0;
    for (int i = 0; i < HISTORY + 5; i++) feed_text("z\r\n");
    assert(hist_len == HISTORY && history[(size_t)hist_head * hist_cols].ch == 'z');
    puts("PASS history: kept lines, wheel and Shift+PgUp scroll back, what is looked at stays, typing returns, "
         "a bounded ring");

    setup_grid(16, 2);
    feed_text("ab cd.e/f  !!x");
    point(4, 0, BTN_LEFT, 1, 2, 0);
    assert(strcmp(copied, "cd.e/f") == 0 && !selection.dragging);
    point(11, 0, BTN_LEFT, 1, 2, 0);
    assert(strcmp(copied, "!!") == 0);
    point(1, 0, BTN_LEFT, 1, 3, 0);
    assert(strcmp(copied, "ab cd.e/f  !!x") == 0);
    point(0, 0, BTN_LEFT, 1, 1, 0);
    point(4, 0, 0, 0, 0, 0);
    point(4, 0, BTN_LEFT, 0, 0, 0);
    assert(strcmp(copied, "ab cd") == 0 && !selection.dragging);
    puts("PASS double click a word, triple click a line, a drag: each copied as the primary selection");

    feed_text("\033[?1049h");
    assert(in_alt);
    point(0, 0, 0, 0, 0, 1);
    output_is("\033[A\033[A\033[A");
    feed_text("\033[?1000h\033[?1006h");
    point(2, 1, 0, 0, 0, -2);
    output_is("\033[<65;3;2M\033[<65;3;2M");
    feed_text("\033[?1000l\033[?1006l\033[?1049l");
    puts("PASS full-screen programs: the wheel as arrow keys, or reported once a step when they track the mouse");
    puts("ALL PASSED");
    return 0;
}
