/*
 * Drive archive (progs/archive.c) by its keys over archives in a scratch
 * directory, with the real archive-worker and the screen, keyboard, files'
 * chooser and home's opening as doubles: browsing the tree, preview, Ark's
 * extraction dialog and defaults, adding, removing, testing, finding, a new
 * encrypted zip and its password, and the file manager's batch actions.
 *   test-archive DIR   (DIR: a new directory; run as a user, not root)
 * Build with base/ui.c and -DARCHIVE_WORKER_TEST_PATH='"WORKER"'.
 */
#define main archive_main
#include "../progs/archive.c"
#include "i18n-stub.h"
#undef main
#include <assert.h>
#include <dirent.h>

#define END     0
#define KEY(k, m) (-1000000 - ((m) << 16) - (k)) /* key k with modifiers m */

static const int *script;
static int step, mods;
static char dir[256], picked[PATH_MAX], opened_with[OPEN_NAME], opened[PATH_MAX];
static struct pointer_event mouse;

void say(const char *fmt, ...) { (void)fmt; }
int screen_open(const char *font) { (void)font; return 1; }
int keys_open(void) { return 1; }
int screen_cols(void) { return 120; }
int screen_rows(void) { return 30; }
int screen_char_cells(uint32_t cp) { (void)cp; return 1; }
void screen_bar(const char *title, const char *ime) { (void)title; (void)ime; }
void screen_fill(int c, int r, int w, int h, uint32_t rgb) { (void)c; (void)r; (void)w; (void)h; (void)rgb; }
void screen_text(int c, int r, const char *s, uint32_t fg, uint32_t bg) { (void)c; (void)r; (void)s; (void)fg; (void)bg; }
void screen_flush(void) {}
const struct pointer_event *pointer_last(void) { return &mouse; }
int keys_modifiers(void) { return mods; }

int key_poll_timeout(int fd, int timeout)
{
    (void)fd;
    (void)timeout;
    mods = 0;
    int key = script[step];
    assert(key != END); /* the script ran out where a key was waited for */
    step++;
    if (key <= -1000000) {
        int raw = -1000000 - key;
        mods = (raw >> 16) & 0xF;
        return raw & 0xFFFF;
    }
    return key;
}

int key_get(void) { return key_poll_timeout(-1, -1); }

/* While the worker works: its output, never a key. */
int key_poll_many(struct pollfd *extra, size_t count, int timeout_ms)
{
    poll(extra, count, timeout_ms);
    return 0;
}

int files_pick(const char *from, const char *keep, const char *type, const char *what, char *out, size_t size)
{
    (void)from;
    (void)keep;
    (void)type;
    (void)what;
    if (!picked[0]) return 0;
    snprintf(out, size, "%s", picked);
    picked[0] = '\0';
    return 1;
}

int open_type(const char *target, char *type, size_t size)
{
    const char *dot = strrchr(target, '.');
    snprintf(type, size, "%s", dot && strcmp(dot, ".txt") == 0 ? "text/plain" : "application/octet-stream");
    return 0;
}

int open_candidates(const char *type, char names[][OPEN_NAME], int max)
{
    if (strcmp(type, "text/plain") != 0 || max < 1) return 0;
    snprintf(names[0], OPEN_NAME, "program:viewer");
    return 1;
}

int open_in_home(const char *with, const char *target)
{
    snprintf(opened_with, sizeof opened_with, "%s", with ? with : "");
    snprintf(opened, sizeof opened, "%s", target);
    return 0;
}

static const char *at(const char *rel)
{
    static char paths[4][PATH_MAX];
    static int i;
    i = (i + 1) % 4;
    snprintf(paths[i], sizeof paths[i], "%s/%s", dir, rel);
    return paths[i];
}

static void sh(const char *cmd)
{
    char line[PATH_MAX * 2];
    snprintf(line, sizeof line, "cd '%s' && %s", dir, cmd);
    assert(system(line) == 0);
}

static int exists(const char *rel)
{
    struct stat st;
    return lstat(at(rel), &st) == 0;
}

static char *text_of(const char *path)
{
    static char buf[256];
    FILE *f = fopen(path, "r");
    size_t n = f ? fread(buf, 1, sizeof buf - 1, f) : 0;
    if (f) fclose(f);
    buf[n] = '\0';
    return buf;
}

/* The keys, as the main loop takes them, until the script ends. */
static void run(const char *what, const int *keys)
{
    printf("== %s\n", what);
    script = keys;
    step = 0;
    quit = 0;
    while (script[step] != END && !quit) {
        draw();
        int key = key_get();
        if (key == K_POINTER) pointer();
        else handle(key | MODS(mods));
    }
}

/* What is shown, for a failing check. */
static void dump(void)
{
    fprintf(stderr, "msg: %s\nrows: %d, stem: %s, format: %s\n", msg, nrows, arc.stem, arc.format);
    for (int i = 0; i < nrows; i++) fprintf(stderr, "  %d %s (%c)\n", depth[i], nodes[rows[i]].name, nodes[rows[i]].type);
}

/* assert, with what is shown when it fails. */
#define check(c) do { if (!(c)) dump(); assert(c); } while (0)

static int row_named(const char *name)
{
    for (int i = 0; i < nrows; i++) {
        if (strcmp(nodes[rows[i]].name, name) == 0) return i;
    }
    return -1;
}

int main(int argc, char **argv)
{
    assert(argc == 2 && geteuid() != 0);
    snprintf(dir, sizeof dir, "%s", argv[1]);
    assert(mkdir(dir, 0755) == 0);
    struct passwd *pw = getpwuid(geteuid());
    user_uid = pw->pw_uid;
    user_gid = pw->pw_gid;
    snprintf(user_home, sizeof user_home, "%s", at("home"));
    sh("mkdir -p home src/docs/deep && echo alpha > src/docs/a.txt && echo beta > src/docs/deep/b.txt && "
       "echo top > src/top.txt && echo new > new.txt && cd src && bsdtar -czf ../multi.tar.gz docs top.txt");

    /* Opened: the top level, folders first; Right opens a folder. */
    check(load_archive(at("multi.tar.gz")) == 0);
    check(nrows == 2 && strcmp(nodes[rows[0]].name, "docs") == 0 && strcmp(arc.stem, "multi") == 0);
    check(arc.files == 3 && arc.dirs == 2 && !arc.readonly && strstr(arc.filters, "gzip"));
    run("open docs, preview a.txt", (const int[]){ K_RIGHT, END });
    check(nrows == 4 && strcmp(nodes[rows[1]].name, "deep") == 0 && strcmp(nodes[rows[2]].name, "a.txt") == 0);
    run("preview", (const int[]){ K_DOWN, K_DOWN, '\n', END });
    check(strcmp(opened_with, "program:viewer") == 0 && strstr(opened, "/home/.cache/locachy/archive/"));
    check(strcmp(text_of(opened), "alpha\n") == 0);
    run("open with: home asks", (const int[]){ 'O', END });
    check(strcmp(opened_with, "ask") == 0);

    /* Ctrl+E: Ark's defaults, a subfolder named as the archive (two things
     * at its top); Down to the buttons, Enter: Extract. */
    run("extract all", (const int[]){ KEY('e' & 0x1F, KEYS_CTRL | KEYS_SHIFT), K_DOWN, K_DOWN, K_DOWN, K_DOWN, K_DOWN,
                                      '\n', END });
    check(exists("multi/docs/deep/b.txt") && exists("multi/top.txt"));
    /* Only what is marked, into a subfolder typed for it. */
    sel = row_named("deep");
    run("extract marked", (const int[]){ ' ', KEY('e' & 0x1F, KEYS_CTRL), K_DOWN, '\n', KEY(21, 0), 'o', 'n', 'l', 'y',
                                         '\n', K_DOWN, K_DOWN, K_DOWN, K_DOWN, '\n', END });
    check(exists("only/docs/deep/b.txt") && !exists("only/top.txt") && !exists("only/docs/a.txt"));
    for (int i = 0; i < nnodes; i++) nodes[i].marked = 0;

    /* Remove from the archive, add into the selected folder. */
    sel = row_named("top.txt");
    run("remove", (const int[]){ K_DEL, 'y', END });
    check(row_named("top.txt") < 0 && find_node("docs/a.txt") > 0);
    snprintf(picked, sizeof picked, "%s", at("new.txt"));
    sel = row_named("deep");
    run("add a file", (const int[]){ KEY('a', KEYS_ALT), '\n', END });
    check(find_node("docs/deep/new.txt") > 0);
    run("test", (const int[]){ KEY('t', KEYS_ALT), END });
    check(strstr(msg, "passed the integrity test"));

    /* Find: what has it in its name, with its folders. */
    run("find", (const int[]){ '/', 'b', '.', 't', '\n', END });
    check(nrows == 3 && row_named("b.txt") == 2);
    run("find gone", (const int[]){ 27, END });
    check(!quit && nrows == 3 && row_named("b.txt") < 0); /* docs open, deep shut, as they were */

    /* Ctrl+N: a zip (Right nine times from tar.gz) with a password, made
     * of top.txt; the password kept for the session, then asked again. */
    snprintf(picked, sizeof picked, "%s", at("src/top.txt"));
    run("new zip", (const int[]){ KEY('n' & 0x1F, KEYS_CTRL), '\n', 's', 'e', 'c', '\n',
                                  K_DOWN, K_RIGHT, K_RIGHT, K_RIGHT, K_RIGHT, K_RIGHT, K_RIGHT, K_RIGHT, K_RIGHT, K_RIGHT,
                                  K_DOWN, K_DOWN, '\n', 'p', 'w', '1', '\n', 'p', 'w', '1', '\n',
                                  K_DOWN, K_DOWN, K_DOWN, '\n', '\n', END });
    check(strstr(arc.path, "/Archivesec.zip") && arc.encrypted && arc.have_password);
    run("preview with the kept password", (const int[]){ '\n', END });
    check(strcmp(text_of(opened), "top\n") == 0);
    arc.have_password = 0;
    run("preview asks", (const int[]){ '\n', 'x', '\n', 'p', 'w', '1', '\n', END });
    check(strcmp(text_of(opened), "top\n") == 0 && arc.have_password);

    /* The file manager's: Extract here (a subfolder as Ark decides), and
     * Compress to "NAME" (made, then added to). */
    char *files[] = { (char *)at("multi.tar.gz") };
    /* Only docs at its top now (top.txt removed): it comes out as it is. */
    check(batch_extract(files, 1, 0, 1, NULL) == 0);
    check(exists("docs/deep/new.txt") && !exists("multi (1)"));
    char *srcs[] = { (char *)at("src/docs"), (char *)at("src/top.txt") };
    check(batch_add(srcs, 2, at("src/both.tar.zst"), 0, NULL) == 0);
    char *more[] = { (char *)at("new.txt") };
    check(batch_add(more, 1, at("src/both.tar.zst"), 0, NULL) == 0);
    check(load_archive(at("src/both.tar.zst")) == 0 && find_node("docs/deep/b.txt") > 0 && find_node("new.txt") > 0);
    check(batch_add(srcs, 2, NULL, 0, "zip") == 0 && exists("src/Archive.zip"));
    check(batch_add(srcs, 2, NULL, 0, "zip") == 0 && exists("src/Archive (1).zip"));

    remove_previews();
    DIR *d = opendir(at("home/.cache/locachy/archive"));
    int left = 0;
    for (struct dirent *e; d && (e = readdir(d)) != NULL;) left += e->d_name[0] != '.';
    if (d) closedir(d);
    check(left == 0);
    puts("ALL PASSED");
    return 0;
}
