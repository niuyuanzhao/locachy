/* files on its own: the file manager home shows, as a program of its own for
 * a managed window on the shared display (process 1's window files). Its
 * screen and keys are opened here, as home opens them for it; home keeps
 * calling files_view() itself on the direct screens. FOLDER: the window's
 * (a folder a container asked to show); none: the home. Ctrl+Q or the
 * window's close ends it; Esc at the top has no list to go back to. */
#include "base.h"
#include "files.h"

int main(int argc, char **argv)
{
    screen_open(BASE_FONT);
    keys_open();
    files_standalone();
    if (argc > 1) {
        files_view_at(argv[1]);
    } else {
        files_view();
    }
    return 0;
}
