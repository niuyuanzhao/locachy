/* edit's text: a gap buffer of UTF-8 bytes with "\n" ending lines (the
 * file's own line ends and encoding are kept beside it, edit-file.c), and the
 * offset of every line's start. Offsets are bytes of the text, not counting
 * the gap. */
#ifndef LOCACHY_EDIT_BUFFER_H
#define LOCACHY_EDIT_BUFFER_H

#include <stddef.h>
#include <stdint.h>

struct buffer {
    char *data;            /* [0, gap) and [gap_end, size): the text */
    size_t gap, gap_end, size;
    size_t *line;          /* line[i]: where line i starts */
    size_t lines, line_size;
};

/* One replacement of buffer_replace_many: from at, len bytes become text
 * (new_len bytes). They come in order and do not overlap. */
struct replacement {
    size_t at, len;
    const char *text;
    size_t new_len;
};

int buffer_init(struct buffer *b);
void buffer_free(struct buffer *b);
size_t buffer_length(const struct buffer *b);
/* All of the text replaced by text: 0, or -1 (no memory, nothing changed). */
int buffer_set(struct buffer *b, const char *text, size_t len);
/* The same with text, malloc'd, taken over (not copied): 0, or -1 (and
 * text freed). */
int buffer_take(struct buffer *b, char *text, size_t len);
int buffer_insert(struct buffer *b, size_t at, const char *text, size_t len);
void buffer_delete(struct buffer *b, size_t at, size_t len);
/* Many replacements in one pass over the text (replace all): 0, or -1. */
int buffer_replace_many(struct buffer *b, const struct replacement *r, size_t n);

int buffer_byte(const struct buffer *b, size_t at); /* -1 past the end */
void buffer_copy(const struct buffer *b, size_t at, size_t len, char *out);
/* A copy of len bytes from at with a NUL after them; NULL if no memory. */
char *buffer_dup(const struct buffer *b, size_t at, size_t len);
/* The bytes from at up to the gap or the end, *len of them, without copying. */
const char *buffer_chunk(const struct buffer *b, size_t at, size_t *len);
/* All of the text in one piece (the gap moved after it). */
const char *buffer_text(struct buffer *b);

size_t buffer_line_start(const struct buffer *b, size_t line);
size_t buffer_line_length(const struct buffer *b, size_t line); /* without its "\n" */
size_t buffer_line_of(const struct buffer *b, size_t at);

/* Characters as screen_text reads them: a valid UTF-8 sequence, else one
 * byte (drawn as "?"). text_decode gives the code point and its bytes. */
uint32_t text_decode(const char *s, size_t len, size_t *bytes);
int text_put(char *out, uint32_t cp); /* UTF-8 of cp: its bytes (1..4) */
/* A mark drawn over the character before it (not a place for the cursor). */
int text_combining(uint32_t cp);
uint32_t buffer_char(const struct buffer *b, size_t at, size_t *bytes);
size_t buffer_next_char(const struct buffer *b, size_t at);
size_t buffer_prev_char(const struct buffer *b, size_t at);
/* The same, over a character and the combining marks after it. */
size_t buffer_next_cluster(const struct buffer *b, size_t at);
size_t buffer_prev_cluster(const struct buffer *b, size_t at);

#endif
