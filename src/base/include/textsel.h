/* Text a program shows, selected with the pointer as in term and copied
 * (textsel.c): what it draws with textsel_put is kept cell by cell, from
 * textsel_frame at the start of each picture. A drag selects from where it
 * was pressed to where it is, a double click a word, a triple click a line;
 * the program offers what was selected as the primary selection when it is
 * made (TEXTSEL_MADE), and to the clipboard with y or Ctrl+C. */
#ifndef LOCACHY_TEXTSEL_H
#define LOCACHY_TEXTSEL_H

#include <stddef.h>
#include <stdint.h>

#include "base.h"

enum { TEXTSEL_NONE, TEXTSEL_ON, TEXTSEL_MADE };

void textsel_frame(void); /* a new picture: nothing shown yet */
/* screen_text, and the text kept where it is shown. */
void textsel_put(int col, int row, const char *s, uint32_t fg, uint32_t bg);
/* The selected cells drawn again in fg on bg (after the picture). */
void textsel_draw(uint32_t fg, uint32_t bg);
/* A pointer event: TEXTSEL_NONE when it is not about a selection (a press
 * starts one only where may_start), TEXTSEL_ON when it is, TEXTSEL_MADE when
 * it made one (a release after a drag, a double or triple click). */
int textsel_pointer(const struct pointer_event *e, int may_start);
/* The selected text, rows without their trailing blanks, joined by newlines
 * (malloc'd), or NULL with nothing selected. */
char *textsel_get(void);
int textsel_active(void);
void textsel_clear(void);
/* text to the primary selection (primary) or the clipboard (clipboard.h);
 * what to tell into msg: why not when it failed (1), what was copied to the
 * clipboard, "" for the primary selection (0). */
int textsel_copy(int primary, const char *text, char *msg, size_t size);

#endif
