/* home's file manager (files.c), also settings' chooser of the wallpaper and
 * packages' of a container's directories. */
#ifndef LOCACHY_FILES_H
#define LOCACHY_FILES_H

/* Show the file manager until the user leaves it. */
void files_view(void);

/* Has a container asked home to show a folder (FILES_REQUEST)? */
int files_request(void);

/* The same, for choosing a file whose type (open_type) starts with type,
 * from folder dir (with the file keep selected): what is said at the start.
 * 1 with its path in out, or 0 (the user went back). Nothing is changed.
 * The type inode/directory chooses a folder (o, on the selected one). */
int files_pick(const char *dir, const char *keep, const char *type, const char *what, char *out, size_t size);

#endif
