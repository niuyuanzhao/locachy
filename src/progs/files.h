/* home's file manager (files.c), also settings' chooser of the wallpaper and
 * packages' of a container's directories. */
#ifndef LOCACHY_FILES_H
#define LOCACHY_FILES_H

/* Show the file manager until the user leaves it. */
void files_view(void);

/* files is a program of its own (files-main), not home's file manager:
 * Esc at the top does nothing, the home key is process 1's, and only Ctrl+Q
 * (or the window's close) leaves. Before files_view or files_view_at. */
void files_standalone(void);

/* The same at folder (or at a file, selected in its folder), checked as a
 * container's request is: only inside the user's home. */
void files_view_at(const char *folder);

/* Has a container asked home to show a folder (FILES_REQUEST), or a program
 * to open a file (OPEN_REQUEST)? Not for one of home's programs to run. */
int files_request(void);

/* Has a program asked home to run one of its programs (open_in_home with
 * "program:NAME" and no target: the top bar's launcher)? 1 with NAME in
 * name, the request taken if take; else 0 (none, another request, or in a
 * window), the request left. Home runs NAME only if its list has it. */
int files_program_request(char *name, size_t size, int take);

/* The same, for choosing a file whose type (open_type) starts with type,
 * from folder dir (with the file keep selected): what is said at the start.
 * 1 with its path in out, or 0 (the user went back). Nothing is changed.
 * The type inode/directory chooses a folder (o, on the selected one). */
int files_pick(const char *dir, const char *keep, const char *type, const char *what, char *out, size_t size);

/* 1 when the last files_view or files_pick ended because quitting was asked
 * for (the window's close, K_CLOSE, or Ctrl+Q and Alt+F4: ACT_QUIT): the
 * caller closes too, as its quit does. */
int files_close_asked(void);

#endif
