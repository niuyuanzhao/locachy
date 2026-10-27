/* Replay real PTY output through LOCachy's term parser, without a display,
 * font, clipboard, input device or a substitute ANSI implementation. */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#define main term_main
#include "../progs/term.c"
#include "i18n-stub.h"
#undef main

int main(int argc, char **argv)
{
    if (argc != 3) return 2;
    cols = atoi(argv[1]); rows = atoi(argv[2]);
    if (cols < 1 || cols > 1000 || rows < 1 || rows > 1000) return 2;
    size_t cells = (size_t)cols * rows;
    grid = calloc(cells, sizeof *grid);
    alt_grid = calloc(cells, sizeof *alt_grid);
    dirty = calloc((size_t)rows, 1);
    soft = calloc((size_t)rows, 1);
    alt_soft = calloc((size_t)rows, 1);
    if (!grid || !alt_grid || !dirty || !soft || !alt_soft) return 1;
    reset_term();
    int byte;
    while ((byte = getchar()) != EOF) feed((uint8_t)byte, -1);
    if (ferror(stdin)) return 1;
    /* The PTY harness sends these real parser replies back to the child. */
    if (output.size && fwrite(output.data, 1, output.size, stderr) != output.size) return 1;
    for (int r = 0; r < rows; r++) {
        for (int c = 0; c < cols; c++) {
            char text[4];
            uint32_t cp = grid[r * cols + c].ch;
            size_t n = put_utf8(text, cp ? cp : ' ');
            if (fwrite(text, 1, n, stdout) != n) return 1;
        }
        putchar('\n');
    }
    return ferror(stdout) ? 1 : 0;
}
