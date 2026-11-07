/* Text shown, to select with the pointer and copy (textsel.h): the
 * installer's pages and packages' panes. A cell: its character's UTF-8, ""
 * for none, "\1" for the right half of a wide one; a place in the selection:
 * row * SHOWN_COLS + col. */
#include <linux/input-event-codes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "base.h"
#include "clipboard.h"
#include "textsel.h"

#define SHOWN_ROWS 160
#define SHOWN_COLS 320
static char (*shown)[SHOWN_COLS][5]; /* from the first picture: a program that draws none has none */
static struct { int active, dragging, anchor, end; } sel;

void textsel_frame(void)
{
    if (!shown) {
        shown = calloc(SHOWN_ROWS, sizeof *shown);
    } else {
        memset(shown, 0, SHOWN_ROWS * sizeof *shown);
    }
}

void textsel_put(int col, int row, const char *s, uint32_t fg, uint32_t bg)
{
    screen_text(col, row, s, fg, bg);
    if (!shown || row < 0 || row >= SHOWN_ROWS) return;
    const unsigned char *p = (const unsigned char *)s;
    while (*p && col < SHOWN_COLS) {
        const unsigned char *from = p;
        unsigned char b = *p++;
        int more = b < 0x80 ? 0 : (b & 0xE0) == 0xC0 ? 1 : (b & 0xF0) == 0xE0 ? 2 : (b & 0xF8) == 0xF0 ? 3 : 0;
        uint32_t cp = b < 0x80 ? b : b & (0x3Fu >> more);
        for (; more > 0 && (*p & 0xC0) == 0x80; more--) cp = cp << 6 | (*p++ & 0x3F);
        int w = screen_char_cells(cp);
        if (col >= 0 && p - from < 5) {
            memcpy(shown[row][col], from, (size_t)(p - from));
            shown[row][col][p - from] = '\0';
        }
        for (int k = 1; k < w && col + k < SHOWN_COLS; k++) {
            if (col + k >= 0) strcpy(shown[row][col + k], "\1");
        }
        col += w > 0 ? w : 1;
    }
}

static int bounds(int *a, int *b)
{
    if (!sel.active || !shown) return 0;
    *a = sel.anchor < sel.end ? sel.anchor : sel.end;
    *b = sel.anchor < sel.end ? sel.end : sel.anchor;
    return 1;
}

void textsel_draw(uint32_t fg, uint32_t bg)
{
    int a, b;
    if (!bounds(&a, &b)) return;
    for (int i = a; i <= b && i / SHOWN_COLS < screen_rows(); i++) {
        const char *ch = shown[i / SHOWN_COLS][i % SHOWN_COLS];
        if (ch[0] && ch[0] != '\1') screen_text(i % SHOWN_COLS, i / SHOWN_COLS, ch, fg, bg);
    }
}

char *textsel_get(void)
{
    int a, b;
    if (!bounds(&a, &b)) return NULL;
    char *text = malloc((size_t)(b - a + 2) * 5 + 1);
    if (!text) return NULL;
    size_t n = 0, line = 0;
    int row = a / SHOWN_COLS;
    for (int i = a; i <= b; i++) {
        if (i / SHOWN_COLS != row) {
            while (n > line && text[n - 1] == ' ') n--;
            text[n++] = '\n';
            line = n;
            row = i / SHOWN_COLS;
        }
        const char *ch = shown[row][i % SHOWN_COLS];
        if (ch[0] == '\1') continue;
        size_t len = ch[0] ? strlen(ch) : 1;
        memcpy(text + n, ch[0] ? ch : " ", len);
        n += len;
    }
    while (n > line && text[n - 1] == ' ') n--;
    text[n] = '\0';
    return text;
}

int textsel_active(void)
{
    return sel.active;
}

void textsel_clear(void)
{
    sel.active = sel.dragging = 0;
}

static int word_cell(int row, int col)
{
    const char *ch = shown[row][col];
    return ch[0] && strcmp(ch, " ") != 0;
}

int textsel_pointer(const struct pointer_event *e, int may_start)
{
    if (!shown || e->row < 0 || e->row >= SHOWN_ROWS || e->col < 0 || e->col >= SHOWN_COLS) return TEXTSEL_NONE;
    int at = e->row * SHOWN_COLS + e->col;
    if (e->button == BTN_LEFT && e->pressed) {
        if (!may_start) return TEXTSEL_NONE;
        sel.active = sel.dragging = 1;
        sel.anchor = sel.end = at;
        if (e->clicks < 2) return TEXTSEL_ON;
        int a = e->col, b = e->col;
        if (e->clicks == 2) {
            while (a > 0 && word_cell(e->row, a - 1)) a--;
            while (b + 1 < SHOWN_COLS && word_cell(e->row, b + 1)) b++;
        } else {
            for (a = 0; a < SHOWN_COLS - 1 && !word_cell(e->row, a); a++) {
            }
            for (b = SHOWN_COLS - 1; b > a && !word_cell(e->row, b); b--) {
            }
        }
        sel.anchor = e->row * SHOWN_COLS + a;
        sel.end = e->row * SHOWN_COLS + b;
        sel.dragging = 0;
        return TEXTSEL_MADE;
    }
    if (!sel.dragging) return TEXTSEL_NONE;
    sel.end = at;
    if (e->button == BTN_LEFT && !e->pressed) {
        sel.dragging = 0;
        if (sel.anchor == sel.end) {
            sel.active = 0; /* a click: no selection */
            return TEXTSEL_ON;
        }
        return TEXTSEL_MADE;
    }
    return TEXTSEL_ON;
}

int textsel_copy(int primary, const char *text, char *msg, size_t size)
{
    int status = clipboard_set(primary, text, strlen(text));
    if (status != CLIPBOARD_OK) {
        snprintf(msg, size, _("Not copied: %s"), _(clipboard_error(status)));
        return 1;
    }
    msg[0] = '\0';
    if (primary) return 0;
    /* Of several lines, how many and the first: a notice is one line. */
    int lines = 1;
    size_t first = strcspn(text, "\n");
    for (const char *s = text; (s = strchr(s, '\n')) != NULL; s++) {
        lines += s[1] != '\0';
    }
    if (lines == 1) {
        snprintf(msg, size, _("Copied: %.200s"), text);
    } else {
        snprintf(msg, size, P_("Copied %d line: %.*s ...", "Copied %d lines: %.*s ...", (unsigned long)lines), lines,
                 (int)(first < 200 ? first : 200), text);
    }
    return 0;
}
