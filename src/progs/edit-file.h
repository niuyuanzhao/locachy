/* edit's files (edit-file.c): read and written with the user's permissions
 * (uid 1000), the text kept as UTF-8 with "\n" line ends and the file's own
 * encoding and line ends beside it, so that saving gives back its bytes. */
#ifndef LOCACHY_EDIT_FILE_H
#define LOCACHY_EDIT_FILE_H

#include <stddef.h>
#include <stdint.h>
#include <sys/stat.h>

#define EDIT_USER 1000

enum { ENC_UTF8, ENC_UTF16LE, ENC_UTF16BE, ENC_GB18030, ENC_LATIN1, ENC_WINDOWS1252, ENC_COUNT };
enum { EOL_LF, EOL_CRLF, EOL_CR };

struct file_format {
    int encoding, bom, eol;
};

/* What loading found besides the text. NUL bytes are none of it: they are
 * valid UTF-8, and KTextEditor loads them as any other character. */
#define FILE_BROKEN  2 /* bytes the encoding cannot read: kept as they are */
#define FILE_WRAPPED 4 /* lines past the line length limit were wrapped */

/* A file's checksum, as KTextEditor's: the git blob SHA-1 of its bytes. */
#define DIGEST_SIZE 20

/* The file system calls that follow are the user's (on), or root's again. */
void file_as_user(int on);

/* Read path as the user: *text (malloc'd, a NUL after its *len bytes), its
 * format, flags and checksum, and its stat (st_ino 0: it is not there; then
 * *text is an empty text and 0 returned, for a new file). Lines longer than
 * limit characters (0: no limit) are wrapped, as KTextEditor's Line Length
 * Limit does, and *longest is the longest of them. 0, or an errno. */
int file_load(const char *path, char **text, size_t *len, struct file_format *format, int *flags,
              struct stat *st, uint8_t digest[DIGEST_SIZE], size_t limit, size_t *longest);

/* As file_load, but encoding is explicit (no fallback). Invalid bytes are
 * retained and FILE_BROKEN is set; callers must keep the document protected. */
int file_load_encoding(const char *path, char **text, size_t *len, struct file_format *format, int *flags,
                       struct stat *st, uint8_t digest[DIGEST_SIZE], size_t limit, size_t *longest, int encoding);

/* The text in format, written as the user to path, through symbolic links
 * to their target as Qt's QSaveFile follows them: a new file beside it,
 * synced, given the old file's mode, and renamed over it. The new file is
 * the user's; the old owner and group are given back where the user may.
 * An existing file must be one the user may write. *st: the file as
 * written; final: the name written (a link's target). 0, or an errno. */
int file_save(const char *path, const char *text, size_t len, const struct file_format *format,
              struct stat *st, char *final, size_t final_size, uint8_t digest[DIGEST_SIZE]);

/* The checksum of the file's bytes as the user reads them now: 0, or an
 * errno. */
int file_digest(const char *path, uint8_t digest[DIGEST_SIZE]);
void file_digest_hex(const uint8_t digest[DIGEST_SIZE], char out[2 * DIGEST_SIZE + 1]);

/* The path a link chain ends at, as QSaveFile follows it (up to 128 links;
 * the target need not exist). */
void file_resolve(const char *path, char *out, size_t size);

/* Name of an encoding and line end, as Kate's status bar shows them. */
const char *file_encoding_name(const struct file_format *format);
const char *file_eol_name(int eol);

#endif
