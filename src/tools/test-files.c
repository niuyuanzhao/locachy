/*
 * Drive home's file manager (files.c) by its keys over a scratch home: the
 * screen, the keyboard and the opening of files are test doubles; the files
 * are real. As root it also checks that the operations are the user's (uid
 * 1000) and that the file permissions return to root's after each.
 *   test-files DIR   (DIR: a new directory, made as the scratch home)
 * The folder request is the real FILES_REQUEST: run it in a private mount
 * namespace with a tmpfs on /run/locachy. The pointer: synthetic events on
 * what the drawing registered. Build with base/ui.c.
 */
#define ARCHIVE_PROGRAM "/bin/true" /* there and runnable: its menus are shown */
#include "../progs/files.c"
#include "i18n-stub.h"
#include <assert.h>

#define END     0
#define TIMEOUT (-1) /* key_poll_timeout times out */
#define REQUEST (-2) /* a container asks for the folder in request; a timeout */
#define OPEN    (-3) /* open_in_home's request: request is the file's text; a timeout */
#define KEY(k, m) (-1000000 - ((m) << 16) - (k)) /* key k with modifiers m */
#define CLICK(i) (-100 - (i)) /* clicks[i], as K_POINTER */

/* A pointer event: on the first cell registered as id, at (col, row) for
 * AT, or on item k of the menu the last right press opened for MENU(k). */
struct click {
    int id, col, row;
    int button, release, clicks, wheel, mods;
};
#define AT (-1)
#define MENU(k) (-10 - (k))
static const struct click *clicks;
static struct pointer_event mouse;
static int mods, menu_col, menu_row, px_fills;
static char clip_paste[256];
const struct pointer_event *pointer_last(void) { return &mouse; }
int keys_modifiers(void) { return mods; }

/* The first cell of what was registered as id when last drawn. */
static void hit_cell(int id, int *col, int *row)
{
    for (*row = 0; *row < 30; (*row)++)
        for (*col = 0; *col < 100; (*col)++)
            if (ui_hit_at(*col, *row) == id) return;
    assert(0);
}

static void point(const struct click *c)
{
    int col = c->col, row = c->row;
    if (c->id <= MENU(0)) {
        col = menu_col + 2;
        row = menu_row + MENU(0) - c->id;
    } else if (c->id >= 0) {
        hit_cell(c->id, &col, &row);
    }
    mouse = (struct pointer_event){ .x = col * 8 + 4, .y = row * 16 + 8, .col = col, .row = row,
                                    .button = c->button, .pressed = c->button && !c->release,
                                    .clicks = c->clicks, .wheel = c->wheel };
    mods = c->mods;
    if (c->button == BTN_RIGHT && !c->release) { menu_col = col; menu_row = row; }
}

static const int *script;
static int step, extra, opens, is_root;
static char scratch[256], request[256];
static char opened_name[OPEN_NAME], opened_target[PATH_MAX], rule[OPEN_NAME];

void say(const char *fmt, ...) { (void)fmt; }
void base_status(const char *tag, char *out, size_t size) { (void)tag; snprintf(out, size, "status"); }
int screen_cols(void) { return 100; }
int screen_rows(void) { return 30; }
void screen_cells(int *w, int *h, int *t) { *w = 8; *h = 16; *t = 0; }
int screen_char_cells(uint32_t cp) { return cp >= 0x4E00 && cp <= 0x9FFF ? 2 : 1; }
void screen_fill(int c, int r, int w, int h, uint32_t rgb) { (void)c; (void)r; (void)w; (void)h; (void)rgb; }
void screen_fill_px(int x, int y, int w, int h, uint32_t rgb)
{
    (void)x; (void)y; (void)w; (void)h; (void)rgb;
    px_fills++;
}
void screen_text(int c, int r, const char *s, uint32_t fg, uint32_t bg)
{ (void)c; (void)r; (void)s; (void)fg; (void)bg; }
void screen_flush(void) {}
void screen_bar(const char *title, const char *ime) { (void)title; (void)ime; }
int rc_set(const char *name, const char *value) { (void)name; (void)value; return 0; }

int clipboard_set(int primary, const void *text, size_t size)
{
    (void)primary;
    size_t n = size < sizeof clip_paste - 1 ? size : sizeof clip_paste - 1;
    memcpy(clip_paste, text, n);
    clip_paste[n] = '\0';
    return CLIPBOARD_OK;
}

int clipboard_get(int primary, int *fd, size_t *size)
{
    (void)primary;
    int p[2];
    if (!clip_paste[0] || pipe2(p, O_CLOEXEC) < 0) return CLIPBOARD_EMPTY;
    *size = strlen(clip_paste);
    assert(write(p[1], clip_paste, *size) == (ssize_t)*size);
    close(p[1]);
    *fd = p[0];
    return CLIPBOARD_OK;
}

int key_poll_timeout(int fd, int timeout)
{
    (void)fd;
    (void)timeout;
    mods = 0;
    int key = script[step];
    if (key == END) {
        assert(++extra < 50); /* Esc does not get out: stuck */
        return 27;
    }
    step++;
    if (key <= -1000000) {
        int raw = -1000000 - key;
        mods = (raw >> 16) & 0xF;
        return raw & 0xFFFF;
    }
    if (key <= CLICK(0)) {
        point(&clicks[CLICK(0) - key]);
        return K_POINTER;
    }
    if (key == REQUEST || key == OPEN) {
        FILE *f = fopen(key == OPEN ? OPEN_REQUEST : FILES_REQUEST, "w");
        assert(f);
        fprintf(f, key == OPEN ? "%s" : "%s\n", request);
        fclose(f);
    }
    return key == TIMEOUT || key == REQUEST || key == OPEN ? 0 : key;
}

void open_user_dirs(char *home_, size_t hsize, char *dirs_, size_t dsize)
{
    snprintf(home_, hsize, "%s", scratch);
    snprintf(dirs_, dsize, "Documents Downloads");
}

int open_type(const char *target, char *type, size_t size)
{
    const char *dot = strrchr(target, '.');
    struct stat st;
    snprintf(type, size, "%s", strncmp(target, "https:", 6) == 0 ? "x-scheme-handler/https" :
             stat(target, &st) == 0 && S_ISDIR(st.st_mode) ? "inode/directory" :
             dot && strcmp(dot, ".txt") == 0 ? "text/plain" :
             dot && strcmp(dot, ".pdf") == 0 ? "application/pdf" :
             dot && strcmp(dot, ".png") == 0 ? "image/png" :
             dot && strcmp(dot, ".zip") == 0 ? "application/zip" : "application/octet-stream");
    return 0;
}

int open_normalize(const char *target, char *out, size_t size)
{
    const char *path = strncmp(target, "file://", 7) == 0 ? target + 7 : target;
    return snprintf(out, size, "%s", path) < (int)size ? 0 : -1;
}

/* "program:viewer" is a framebuffer program: a task of its own (base_open). */
static char task[2][PATH_MAX];
static int tasks;
int open_program(const char *name, const char *target, char *spec, size_t size, char *argv[], int max)
{
    if (strcmp(name, "program:viewer") != 0 || max < 3) {
        return 0;
    }
    snprintf(spec, size, "/usr/local/bin/viewer");
    argv[0] = spec;
    argv[1] = (char *)target;
    argv[2] = NULL;
    return 2;
}

void base_open(char *const argv[], char *why, size_t size)
{
    tasks++;
    snprintf(task[0], sizeof task[0], "%s", argv[0]);
    snprintf(task[1], sizeof task[1], "%s", argv[1] ? argv[1] : "");
    snprintf(why, size, "back viewer");
}

/* archive, called by the Extract and Compress menus: its argv, and the
 * status it ends with. */
static char called[8][PATH_MAX];
static int ncalled, call_status;
int base_call(char *const argv[])
{
    for (ncalled = 0; argv[ncalled] && ncalled < 8; ncalled++) {
        snprintf(called[ncalled], sizeof called[0], "%s", argv[ncalled]);
    }
    return call_status;
}

/* "editor" is a host program: it sees the whole home. */
int open_host(const char *name) { return strcmp(name, "editor") == 0; }

int open_shared(const char *path, char *why, size_t size)
{
    char docs[300], downs[300];
    snprintf(docs, sizeof docs, "%s/Documents", scratch);
    snprintf(downs, sizeof downs, "%s/Downloads", scratch);
    if (inside(path, docs) || inside(path, downs)) {
        return 1;
    }
    snprintf(why, size, "%.400s: not in a shared folder", path);
    return 0;
}

static int fill(char names[][OPEN_NAME], int max, const char *const *from)
{
    int n = 0;
    for (; from[n] && n < max; n++) {
        snprintf(names[n], OPEN_NAME, "%s", from[n]);
    }
    return n;
}

int open_candidates(const char *type, char names[][OPEN_NAME], int max)
{
    static const char *const editors[] = { "one", "two", NULL }, *const none[] = { NULL };
    static const char *const viewers[] = { "program:viewer", NULL }, *const browsers[] = { "browser", NULL };
    static const char *const archivers[] = { "program:archive", NULL };
    return fill(names, max, strcmp(type, "text/plain") == 0 ? editors :
                            strcmp(type, "application/zip") == 0 ? archivers :
                            strcmp(type, "application/pdf") == 0 ? viewers :
                            strcmp(type, "x-scheme-handler/https") == 0 ? browsers : none);
}

int open_containers(char names[][OPEN_NAME], int max)
{
    static const char *const all[] = { "one", "three", "two", NULL };
    return fill(names, max, all);
}

void open_label(const char *name, const char *type, char *label, size_t size)
{
    (void)type;
    snprintf(label, size, "Test %.60s", name);
}

int open_rule(const char *type, char *name, size_t size)
{
    if (strcmp(type, "text/plain") != 0 || !rule[0]) {
        return OPEN_NO_RULE;
    }
    if (strcmp(rule, "ask") == 0) {
        return OPEN_RULE_ASK;
    }
    snprintf(name, size, "%s", rule);
    return OPEN_RULE_CONTAINER;
}

int open_set_rule(const char *type, const char *value)
{
    assert(strcmp(type, "text/plain") == 0);
    snprintf(rule, sizeof rule, "%s", value ? value : "");
    return 0;
}

int base_app_open(const char *name, const char *target, char *why, size_t size)
{
    (void)why;
    (void)size;
    opens++;
    snprintf(opened_name, sizeof opened_name, "%s", name);
    snprintf(opened_target, sizeof opened_target, "%s", target);
    return 0;
}

/* scratch/rel; four may be in use at once. */
static const char *at_home(const char *rel)
{
    static char path[4][PATH_MAX];
    static int i;
    i = (i + 1) % 4;
    snprintf(path[i], PATH_MAX, "%s/%s", scratch, rel);
    return path[i];
}

static int exists(const char *rel)
{
    struct stat st;
    return lstat(at_home(rel), &st) == 0;
}

static uid_t owner(const char *rel)
{
    struct stat st;
    assert(lstat(at_home(rel), &st) == 0);
    return st.st_uid;
}

static void own(const char *rel)
{
    if (is_root) {
        assert(chown(at_home(rel), USER, USER) == 0);
    }
}

static void mk(const char *rel)
{
    assert(mkdir(at_home(rel), 0755) == 0);
    own(rel);
}

static void put(const char *rel, time_t mtime)
{
    FILE *f = fopen(at_home(rel), "w");
    assert(f);
    fputs(rel, f);
    fclose(f);
    own(rel);
    if (mtime) {
        struct timespec t[2] = { { mtime, 0 }, { mtime, 0 } };
        assert(utimensat(AT_FDCWD, at_home(rel), t, 0) == 0);
    }
}

/* files_view with these keys; all of them must be used. */
static void run(const char *what, const int *keys)
{
    printf("== %s\n", what);
    script = keys;
    step = extra = 0;
    files_view();
    assert(script[step] == END);
}

/* files_pick of a file of type from dir, keep selected, with these keys. */
static int pick(const char *what, const char *type, const char *dir, const char *keep, const int *keys,
                char *out, size_t size)
{
    printf("== %s\n", what);
    script = keys;
    step = extra = 0;
    int chosen = files_pick(dir, keep, type, "Choose one.", out, size);
    assert(script[step] == END && pick_type == NULL);
    return chosen;
}

static int here(const char *rel)
{
    return strcmp(P->cwd, rel[0] ? at_home(rel) : scratch) == 0;
}

static int index_of(const char *name)
{
    for (int i = 0; i < P->nlist; i++) if (strcmp(P->list[i].name, name) == 0) return i;
    assert(0);
    return -1;
}

int main(int argc, char **argv)
{
    assert(argc == 2);
    is_root = geteuid() == 0;
    assert(mkdir(argv[1], 0755) == 0);
    char *real = realpath(argv[1], NULL);
    assert(real && strlen(real) < 200);
    snprintf(scratch, sizeof scratch, "%s", real);
    free(real);
    own("");
    mk("Documents");
    mk("Downloads");
    mk("Documents/sub");
    mk("Documents/sub/deep");
    put("Documents/sub/deep/x", 0);
    put("Documents/a.txt", 0);
    put("Documents/b.txt", 0);
    put("Documents/.hidden", 0);
    put("notes.txt", 0);
    unlink(FILES_REQUEST);

    /* The home: Documents/ Downloads/ notes.txt; Documents: sub/ a.txt b.txt. */
    run("open: two can, the chooser, once", (const int[]){ '\n', K_DOWN, '\n', '2', '\n', END });
    assert(opens == 1 && strcmp(opened_name, "two") == 0);
    assert(strcmp(opened_target, at_home("Documents/a.txt")) == 0 && !rule[0]);

    run("open with: A makes it the rule", (const int[]){ K_DOWN, 'o', 'a', END });
    assert(opens == 2 && strcmp(opened_name, "one") == 0 && strcmp(rule, "one") == 0);

    char kept[PATH_MAX];
    snprintf(kept, sizeof kept, "%s", P->cwd);
    run("the home key: back to home's list at once, the place kept", (const int[]){ K_ACT_HOME, END });
    assert(extra == 0 && strcmp(P->cwd, kept) == 0);

    run("open: the rule, no chooser", (const int[]){ K_DOWN, K_DOWN, '\n', END });
    assert(opens == 3 && strcmp(opened_name, "one") == 0);
    assert(strcmp(opened_target, at_home("Documents/b.txt")) == 0);

    run("open: not in a shared folder", (const int[]){ K_LEFT, K_EOL, '\n', END });
    assert(opens == 3 && error && strstr(msg, "containers cannot see it") && here(""));

    struct stat src_st, copy_st;
    assert(stat(at_home("Documents/a.txt"), &src_st) == 0 && chmod(at_home("Documents/a.txt"), 06755) == 0);
    run("copy, paste twice", (const int[]){ '\n', K_DOWN, 'c', K_LEFT, K_DOWN, '\n', 'v', 'v', END });
    assert(here("Downloads") && exists("Downloads/a.txt") && exists("Downloads/a (2).txt"));
    assert(exists("Documents/a.txt") && !error);
    assert(!is_root || (owner("Downloads/a.txt") == USER && owner("Downloads/a (2).txt") == USER));
    /* As cp without -p: no setuid or setgid bit with the copy, the rest kept. */
    assert(stat(at_home("Downloads/a.txt"), &copy_st) == 0 && (copy_st.st_mode & 07000) == 0 &&
           (copy_st.st_mode & 0100));
    assert(chmod(at_home("Documents/a.txt"), src_st.st_mode & 07777) == 0);

    /* Downloads: a (2).txt, a.txt */
    run("cut, paste: moved", (const int[]){ 'x', K_LEFT, K_BOL, '\n', 'v', END });
    assert(exists("Documents/a (2).txt") && !exists("Downloads/a (2).txt") && nclip == 0);

    /* Documents: sub/ a (2).txt a.txt b.txt */
    run("mark two, delete permanently (Shift+Delete): no, then yes",
        (const int[]){ K_DOWN, ' ', K_DOWN, ' ', 'D', 'n', 'D', 'y', END });
    assert(!exists("Documents/a (2).txt") && !exists("Documents/b.txt") && exists("Documents/a.txt"));

    run("rename, new folder",
        (const int[]){ K_DOWN, 'r', '\b', '\b', '\b', '\b', '\b', 'c', '.', 'm', 'd', '\n',
                       'n', 'n', 'e', 'w', '\n', END });
    assert(exists("Documents/c.md") && !exists("Documents/a.txt") && exists("Documents/new"));
    assert(!is_root || owner("Documents/new") == USER);

    /* Documents: new/ sub/ c.md */
    run("a folder is not moved into itself", (const int[]){ 'x', '\n', 'v', END });
    assert(here("Documents/new") && exists("Documents/new") && error);

    run("delete a folder with what is in it", (const int[]){ K_LEFT, K_DOWN, 'D', 'y', END });
    assert(!exists("Documents/sub") && here("Documents"));

    if (is_root) {
        /* Root's: the user cannot delete it, and root's permissions come back. */
        assert(mkdir(at_home("Documents/locked"), 0755) == 0);
        FILE *f = fopen(at_home("Documents/locked/f"), "w");
        assert(f);
        fclose(f);
        run("the user's permissions: root's files stay", (const int[]){ 'D', 'y', END });
        assert(exists("Documents/locked/f") && error);
        assert(unlink(at_home("Documents/locked/f")) == 0 && rmdir(at_home("Documents/locked")) == 0);
    }

    put("Downloads/zz-old.txt", 1000000000);
    put("Downloads/newest.txt", time(NULL) + 60);
    snprintf(request, sizeof request, "%s", at_home("Downloads"));
    FILE *f = fopen(FILES_REQUEST, "w");
    assert(f);
    fprintf(f, "%s\n", request);
    fclose(f);
    assert(files_request());
    run("a container's folder request: there, the newest file selected", (const int[]){ '\n', END });
    assert(here("Downloads") && !files_request() && opens == 4);
    assert(strcmp(opened_target, at_home("Downloads/newest.txt")) == 0);

    snprintf(request, sizeof request, "%s", at_home("Downloads/zz-old.txt"));
    run("a container's file request: its folder shown, that file selected", (const int[]){ REQUEST, END });
    assert(here("Downloads") && strcmp(P->list[P->sel].name, "zz-old.txt") == 0);

    snprintf(request, sizeof request, "%s", at_home("Documents"));
    run("a request while shown; no candidates: every container",
        (const int[]){ REQUEST, '\n', K_DOWN, '\n', END });
    assert(here("Documents") && opens == 5 && strcmp(opened_name, "three") == 0);
    assert(strcmp(opened_target, at_home("Documents/c.md")) == 0);

    snprintf(request, sizeof request, "/etc");
    run("a request outside the home is not shown", (const int[]){ REQUEST, END });
    assert(here("Documents") && !files_request() && error);

    /* Documents: new/ c.md .hidden */
    run("filter", (const int[]){ '/', 'c', '\n', 'q', END });
    assert(P->nlist == 1 && strcmp(P->list[0].name, "c.md") == 0);
    run("hidden files", (const int[]){ '.', 'q', END });
    assert(P->nlist == 3);
    run("hidden files off", (const int[]){ '.', 'q', END });
    assert(P->nlist == 2);

    run("places", (const int[]){ '\t', K_DOWN, K_DOWN, '\n', 'q', END });
    assert(here("Downloads"));

    /* Choosing (settings' wallpaper): only a file of the type; nothing changes. */
    char chosen[PATH_MAX] = "";
    assert(pick("choose: not this one, nor d or x, a folder is gone into, then a text", "text/",
                at_home("Documents"), "c.md",
                (const int[]){ '\n', 'd', 'x', K_BOL, '\n', K_LEFT, '\t', K_DOWN, K_DOWN, '\n', K_BOL, '\n', END },
                chosen, sizeof chosen) == 1);
    assert(strcmp(chosen, at_home("Downloads/a.txt")) == 0);
    assert(exists("Documents/c.md") && exists("Documents/new") && nclip == 0);
    chosen[0] = '\0';
    assert(pick("choose: Esc, nothing chosen", "text/", at_home("Downloads"), NULL, (const int[]){ 27, END },
                chosen, sizeof chosen) == 0 && !chosen[0]);
    assert(pick("choose: the home key leaves the choice open (only Esc ends it)", "text/", at_home("Downloads"),
                NULL, (const int[]){ K_ACT_HOME, END }, chosen, sizeof chosen) == 0 && extra == 1);

    /* Choosing a folder (packages: a container's directories): o on it. */
    assert(pick("choose a folder: o on a file is refused, nor d; Enter goes in, o chooses",
                "inode/directory", at_home("Documents"), "c.md",
                (const int[]){ 'o', 'd', K_BOL, '\n', K_LEFT, 'o', END }, chosen, sizeof chosen) == 1);
    assert(strcmp(chosen, at_home("Documents/new")) == 0 && exists("Documents/new") && exists("Documents/c.md"));

    /* The pointer. Documents: new/ c.md; home: Documents/ Downloads/ notes.txt. */
    nclip = 0;
    struct click tour[] = {
        { .id = HIT_PLACE + 0, .button = BTN_LEFT, .clicks = 1 },
        { .id = HIT_FILE + 0, .button = BTN_LEFT, .clicks = 2 },
        { .id = HIT_PATH + (int)strlen(scratch), .button = BTN_LEFT, .clicks = 1 },
        { .id = HIT_FILE + 0, .button = BTN_LEFT, .clicks = 2 },
        { .id = HIT_FILE + 1, .button = BTN_RIGHT, .clicks = 1 },
        { .id = HIT_KEY + 'v', .button = BTN_LEFT, .clicks = 1 },
        { .id = AT, .col = SIDE + 10, .row = 12, .button = BTN_RIGHT, .clicks = 1 },
        { .id = AT, .col = SIDE + 10, .row = 12, .button = BTN_RIGHT, .release = 1 },
        { .id = MENU(0) },
        { .id = MENU(0), .button = BTN_LEFT, .clicks = 1 },
        { .id = MENU(0), .button = BTN_LEFT, .release = 1 },
        { .id = HIT_KEY + 'q', .button = BTN_LEFT, .clicks = 1 },
    };
    clicks = tour;
    run("pointer: a place, double clicks, a folder of the path, a menu by keys, a button, "
        "a menu by the pointer (its opening release chooses nothing)",
        (const int[]){ CLICK(0), CLICK(1), CLICK(2), CLICK(3), CLICK(4), K_DOWN, K_DOWN, '\n', CLICK(5),
                       CLICK(6), CLICK(7), CLICK(8), CLICK(9), CLICK(10), 'p', '\n', CLICK(11), END });
    assert(here("Documents") && exists("Documents/c (2).md") && exists("Documents/p") && nclip == 1);

    /* Documents: new/ p/ c (2).md c.md */
    int inew = index_of("new"), ip = index_of("p"), ic2 = index_of("c (2).md");
    struct click marking[] = {
        { .id = HIT_FILE + inew, .button = BTN_LEFT, .clicks = 1 },
        { .id = HIT_FILE + ip, .button = BTN_LEFT, .clicks = 1, .mods = KEYS_SHIFT },
        { .id = HIT_FILE + inew, .button = BTN_LEFT, .clicks = 1, .mods = KEYS_CTRL },
        { .id = HIT_FILE + ic2, .button = BTN_LEFT, .clicks = 1, .mods = KEYS_CTRL },
        { .id = AT, .col = 5, .row = 5 },
        { .id = HIT_KEY + 'n', .button = BTN_LEFT, .clicks = 1 },
        { .id = HIT_KEY + 'y', .button = BTN_LEFT, .clicks = 1 },
        { .id = HIT_FILE + 0, .wheel = -5 },
    };
    clicks = marking;
    run("pointer: Shift marks a range, Ctrl one; delete answered by buttons (a motion is no answer); the wheel",
        (const int[]){ CLICK(0), CLICK(1), CLICK(2), CLICK(3), 'D', CLICK(4), CLICK(5), 'D', CLICK(6),
                       CLICK(7), 'q', END });
    assert(!exists("Documents/p") && !exists("Documents/c (2).md"));
    assert(exists("Documents/new") && exists("Documents/c.md") && P->sel == P->nlist - 1);

    int ic = index_of("c.md");
    struct click opening[] = {
        { .id = HIT_FILE + ic, .button = BTN_RIGHT, .clicks = 1 },
        { .id = HIT_FILE + 1, .button = BTN_LEFT, .clicks = 2 },
        { .id = HIT_KEY + 27, .button = BTN_LEFT, .clicks = 1 },
    };
    clicks = opening;
    run("pointer: open with from the menu (no candidates: every container), chosen by a double click; "
        "the chooser's cancel button",
        (const int[]){ CLICK(0), K_DOWN, '\n', CLICK(1), 'o', CLICK(2), 'q', END });
    assert(opens == 6 && strcmp(opened_name, "three") == 0 && strcmp(opened_target, at_home("Documents/c.md")) == 0);
    assert(strstr(msg, "Not opened") && strcmp(rule, "one") == 0);

    /* A host program is not held to the shared folders. Home: notes.txt last. */
    snprintf(rule, sizeof rule, "editor");
    run("open: a host program opens a file outside the shared folders", (const int[]){ K_LEFT, K_EOL, '\n', END });
    assert(opens == 7 && strcmp(opened_name, "editor") == 0 && strcmp(opened_target, at_home("notes.txt")) == 0);
    rule[0] = '\0';

    /* Framebuffer programs, and what programs on the screen ask home to open
     * (open_in_home). Home: Documents/ Downloads/ book.pdf notes.txt. */
    put("book.pdf", 0);
    run("open: the one candidate is a framebuffer program: a task, outside the shared folders too",
        (const int[]){ K_EOL, K_UP, '\n', END });
    assert(tasks == 1 && opens == 7 && strcmp(task[0], "/usr/local/bin/viewer") == 0);
    assert(strcmp(task[1], at_home("book.pdf")) == 0 && !error);

    snprintf(request, sizeof request, "\nhttps://example.org/a\n");
    run("a program's link: a URL, opened by its one candidate, not held to the shared folders",
        (const int[]){ OPEN, END });
    assert(opens == 8 && strcmp(opened_name, "browser") == 0 && strcmp(opened_target, "https://example.org/a") == 0);
    assert(!files_request() && !error);

    snprintf(request, sizeof request, "program:viewer\nfile://%s\n", at_home("Documents/c.md"));
    run("a container's file for a framebuffer program: its folder shown, it selected, opened with it",
        (const int[]){ OPEN, END });
    assert(tasks == 2 && strcmp(task[1], at_home("Documents/c.md")) == 0);
    assert(here("Documents") && strcmp(P->list[P->sel].name, "c.md") == 0 && !files_request());

    snprintf(request, sizeof request, "program:nothing\n%s\n", at_home("book.pdf"));
    run("a request with a program there is not: an error, nothing opened", (const int[]){ OPEN, END });
    assert(tasks == 2 && opens == 8 && error && strstr(msg, "no program"));

    /*
     * FreeDesktop.org Trash 1.0 lifecycle: Delete moves to ~/.local/share/
     * Trash/{files,info} with URL-escaped Path= and DeletionDate=, collisions
     * get .2, trash:// shows them, r restores, Ctrl+Z undoes, e empties.
     */
    put("Documents/space name.txt", 0);
    run("Delete moves to Trash with .trashinfo",
        (const int[]){ '\n', K_EOL, K_DEL, END });
    assert(!exists("Documents/space name.txt") && exists(".local/share/Trash/files/space name.txt"));
    assert(exists(".local/share/Trash/info/space name.txt.trashinfo"));
    assert(!is_root || owner(".local/share/Trash/info/space name.txt.trashinfo") == USER);
    FILE *ti = fopen(at_home(".local/share/Trash/info/space name.txt.trashinfo"), "r");
    assert(ti);
    char info_buf[512] = "";
    assert(fread(info_buf, 1, sizeof info_buf - 1, ti) > 0);
    fclose(ti);
    assert(strstr(info_buf, "[Trash Info]\nPath=") && strstr(info_buf, "Documents/space%20name.txt\n"));
    assert(strstr(info_buf, "DeletionDate=20"));

    /* Same name trashed again: .2 collision handling, then Ctrl+Z undoes it. */
    put("Documents/space name.txt", 0);
    run("Trash collision (.2) and Ctrl+Z undo",
        (const int[]){ K_EOL, 'd', END });
    assert(!exists("Documents/space name.txt") && exists(".local/share/Trash/files/space name.2.txt"));
    run("Ctrl+Z restores the last trashed item",
        (const int[]){ KEY('z' & 0x1F, KEYS_CTRL), END });
    assert(exists("Documents/space name.txt") && !exists(".local/share/Trash/files/space name.2.txt"));
    assert(!exists(".local/share/Trash/info/space name.2.txt.trashinfo"));

    /* Inside trash://: restore ('r') refuses to overwrite an existing file, then succeeds. */
    run("trash:// place, restore refuses to overwrite existing file",
        (const int[]){ '\t', K_DOWN, K_DOWN, K_DOWN, '\n', 'r', END });
    assert(strcmp(P->cwd, TRASH_URL) == 0 && P->nlist == 1 && error && strstr(msg, "there already"));
    assert(unlink(at_home("Documents/space name.txt")) == 0);
    run("restore succeeds once original path is free",
        (const int[]){ 'r', END });
    assert(strcmp(P->cwd, TRASH_URL) == 0 && P->nlist == 0 && !error);
    assert(exists("Documents/space name.txt") && !exists(".local/share/Trash/files/space name.txt"));

    put("Documents/ doomed.txt", 0);
    run("trash and empty Trash",
        (const int[]){ '\t', K_DOWN, '\n', K_DOWN, 'd', '\t', K_DOWN, K_DOWN, K_DOWN, '\n', 'e', 'y', END });
    assert(strcmp(P->cwd, TRASH_URL) == 0 && P->nlist == 0 && !exists(".local/share/Trash/files/ doomed.txt"));

    /* Undo for New Folder, Rename, Cut+Paste Move, and Copy+Paste. */
    run("undo new folder, rename, move, copy",
        (const int[]){ '\t', K_DOWN, '\n',
                       KEY(K_F1 + 9, 0), 'u', 'd', 'i', 'r', '\n', 'u',
                       K_DOWN, KEY(K_F1 + 1, 0), '\b', '\b', '\b', 'b', 'a', 'k', '\n', 'u',
                       KEY('c' & 0x1F, KEYS_CTRL), KEY('v' & 0x1F, KEYS_CTRL), 'u', 'y', END });
    assert(here("Documents") && !exists("Documents/udir") && exists("Documents/c.md") && !exists("Documents/c.bak"));
    assert(!exists("Documents/c (2).md"));

    /* Views (Icons Ctrl+1, Compact Ctrl+2, Details Ctrl+3), zoom, and sorting. */
    put("Documents/pic.png", 2000000000);
    for (int i = 0; i < 12; i++) {
        char rel[64];
        snprintf(rel, sizeof rel, "Documents/item-%02d.txt", i + 1);
        put(rel, 1500000000 + i);
    }
    px_fills = 0;
    run("Icons view (Ctrl+1), grid navigation, zoom (Ctrl++, Ctrl+-, Ctrl+0), Compact (Ctrl+2), Details (Ctrl+3)",
        (const int[]){ KEY(K_CTRL_0 + 1, 0), K_RIGHT, K_RIGHT, K_DOWN, K_LEFT,
                       KEY('+', KEYS_CTRL), KEY('-', KEYS_CTRL), KEY(K_CTRL_0, 0),
                       KEY(K_CTRL_0 + 2, 0), K_DOWN, KEY(K_CTRL_0 + 3, 0), END });
    assert(P->view == VIEW_DETAILS && px_fills > 20);

    struct click sorts[] = {
        { .id = HIT_SORT + SORT_SIZE, .button = BTN_LEFT, .clicks = 1 },
        { .id = HIT_SORT + SORT_SIZE, .button = BTN_LEFT, .clicks = 1 },
        { .id = HIT_SORT + SORT_MTIME, .button = BTN_LEFT, .clicks = 1 },
        { .id = HIT_SORT + SORT_TYPE, .button = BTN_LEFT, .clicks = 1 },
        { .id = HIT_SORT + SORT_NAME, .button = BTN_LEFT, .clicks = 1 },
    };
    clicks = sorts;
    run("Details column headers sort ascending and descending (folders first)",
        (const int[]){ CLICK(0), CLICK(1), CLICK(2), CLICK(3), CLICK(4), END });
    assert(P->list[0].dir && strcmp(P->list[0].name, "new") == 0 && P->sort == SORT_NAME && !P->desc);

    /*
     * Mouse rubberband marquee selection and cross-screen auto-scrolling drag
     * across >1 screen of files (50 items on a 30-row screen).
     */
    mk("Documents/many");
    for (int i = 0; i < 50; i++) {
        char rel[64];
        snprintf(rel, sizeof rel, "Documents/many/f-%02d.txt", i + 1);
        put(rel, 0);
    }
    struct click band[] = {
        /* Press in empty area on row 3, drag to row 26 (bottom row of pane), then release. */
        { .id = AT, .col = SIDE + 70, .row = 3, .button = BTN_LEFT, .clicks = 1 },
        { .id = AT, .col = SIDE + 10, .row = 26 },
        { .id = AT, .col = SIDE + 10, .row = 26, .button = BTN_LEFT, .release = 1 },
        /* Scroll back up with a rubberband from row 26 to row 3. */
        { .id = AT, .col = SIDE + 70, .row = 26, .button = BTN_LEFT, .clicks = 1 },
        { .id = AT, .col = SIDE + 10, .row = 3 },
        { .id = AT, .col = SIDE + 10, .row = 3, .button = BTN_LEFT, .release = 1 },
    };
    clicks = band;
    run("rubberband marquee and cross-screen auto-scroll down across 50 files",
        (const int[]){ '\n', CLICK(0), CLICK(1),
                       TIMEOUT, TIMEOUT, TIMEOUT, TIMEOUT, TIMEOUT, TIMEOUT, TIMEOUT, TIMEOUT,
                       TIMEOUT, TIMEOUT, TIMEOUT, TIMEOUT, TIMEOUT, TIMEOUT, TIMEOUT, TIMEOUT,
                       TIMEOUT, TIMEOUT, TIMEOUT, TIMEOUT, TIMEOUT, TIMEOUT, TIMEOUT, TIMEOUT,
                       TIMEOUT, TIMEOUT, TIMEOUT, TIMEOUT, TIMEOUT, TIMEOUT,CLICK(2), END });
    assert(here("Documents/many") && P->top > 20 && marked(P) == 50);

    run("cross-screen auto-scroll back up to the top",
        (const int[]){ CLICK(3), CLICK(4),
                       TIMEOUT, TIMEOUT, TIMEOUT, TIMEOUT, TIMEOUT, TIMEOUT, TIMEOUT, TIMEOUT,
                       TIMEOUT, TIMEOUT, TIMEOUT, TIMEOUT, TIMEOUT, TIMEOUT, TIMEOUT, TIMEOUT,
                       TIMEOUT, TIMEOUT, TIMEOUT, TIMEOUT, TIMEOUT, TIMEOUT, TIMEOUT, TIMEOUT,
                       TIMEOUT, TIMEOUT, TIMEOUT, TIMEOUT, TIMEOUT, TIMEOUT, CLICK(5), K_LEFT, END });
    assert(here("Documents") && P->top == 0);
    assert(remove_tree(at_home("Documents/many")) == 0);

    /* Drag and drop: into a folder, and onto the Trash place in the sidebar. */
    run("reload Documents", (const int[]){ KEY(K_F1 + 4, 0), END });
    int item1 = index_of("item-01.txt"), item2 = index_of("item-02.txt");
    inew = index_of("new");
    struct click dnd[] = {
        { .id = HIT_FILE + item1, .button = BTN_LEFT, .clicks = 1 },
        { .id = HIT_FILE + inew },
        { .id = HIT_FILE + inew, .button = BTN_LEFT, .release = 1 },
        { .id = HIT_FILE + item2 - 1, .button = BTN_LEFT, .clicks = 1 },
        { .id = HIT_PLACE + nplaces - 1 },
        { .id = HIT_PLACE + nplaces - 1, .button = BTN_LEFT, .release = 1 },
    };
    clicks = dnd;
    run("drag and drop: into a folder, and onto Trash in the sidebar",
        (const int[]){ CLICK(0), CLICK(1), CLICK(2), CLICK(3), CLICK(4), CLICK(5), END });
    assert(exists("Documents/new/item-01.txt") && !exists("Documents/item-01.txt"));
    assert(!exists("Documents/item-02.txt") && exists(".local/share/Trash/files/item-02.txt"));

    /*
     * Navigation up to / and Back/Forward history (Alt+Left, Alt+Right, Alt+Up),
     * Ctrl+L location bar with Ctrl+V clipboard paste, Split view (F3), Tabs
     * (Ctrl+T, Ctrl+Tab, Ctrl+W), and Properties (i / Alt+Return, x chmod).
     */
    run("navigate up to / and back/forward with Alt+Left/Right",
        (const int[]){ KEY(K_UP, KEYS_ALT), KEY(K_UP, KEYS_ALT), KEY(K_UP, KEYS_ALT),
                       KEY(K_UP, KEYS_ALT), KEY(K_LEFT, KEYS_ALT), KEY(K_LEFT, KEYS_ALT),
                       KEY(K_LEFT, KEYS_ALT), KEY(K_RIGHT, KEYS_ALT), KEY(K_LEFT, KEYS_ALT), END });
    assert(here("Documents"));

    snprintf(clip_paste, sizeof clip_paste, "%s/Downloads", scratch);
    run("Ctrl+L location bar with Ctrl+V clipboard paste, Tabs (Ctrl+T, Ctrl+Tab, Ctrl+W), Split (F3)",
        (const int[]){ KEY('l' & 0x1F, KEYS_CTRL), KEY('v' & 0x1F, KEYS_CTRL), '\n',
                       KEY('t' & 0x1F, KEYS_CTRL), K_LEFT,
                       KEY('\t', KEYS_CTRL), KEY('\t', KEYS_CTRL), KEY('w' & 0x1F, KEYS_CTRL),
                       KEY(K_F1 + 2, 0), '\t', '\t', KEY(K_LEFT, KEYS_ALT), END });
    assert(tabs[cur_tab].split == 1 && ntabs == 1);
    assert(strcmp(tabs[0].pane[0].cwd, at_home("Documents")) == 0);
    assert(strcmp(tabs[0].pane[1].cwd, at_home("Downloads")) == 0);

    /* Properties: toggle executable bit on c.md, octal chmod (m), Folder Properties (I), then close split pane. */
    run("Properties dialog (i), executable toggle (x), octal chmod (m), Folder Properties (I)",
        (const int[]){ K_DOWN, 'i', 'x', 'm', '7', '5', '0', '\n', 27, 'I', 27, KEY(K_F1 + 2, 0), END });
    struct stat st;
    assert(stat(at_home("Documents/c.md"), &st) == 0 && (st.st_mode & 0777) == 0750);
    assert(tabs[cur_tab].split == 0);
    go(at_home("Documents"), NULL);

    /* PCManFM features: live status bar metadata, Ctrl+Alt+1..5 & 's' sort menu
     * (By Extension, Mingle files and folders), 'C' columns menu + files.conf,
     * New empty file (N) + undo, Symlink (L) + undo, Duplicate (Ctrl+U),
     * Copy path (Ctrl+Shift+C), Invert selection (Ctrl+Shift+I), Bookmarks
     * (Ctrl+D in ~/.config/gtk-3.0/bookmarks), F9 side pane, F4 terminal,
     * and Ctrl+Shift+F recursive Find Files. */
    char sbuf[512] = "";
    format_status_line(P, sbuf, sizeof sbuf);
    assert(strstr(sbuf, "items") && strstr(sbuf, "free"));

    run("PCManFM shortcuts: Ctrl+Alt+5 sort by extension, 's' menu mingle dirs, 'C' columns",
        (const int[]){ KEY('5', KEYS_CTRL | KEYS_ALT),
                       's', K_DOWN, K_DOWN, K_DOWN, K_DOWN, K_DOWN, K_DOWN, K_DOWN, K_DOWN, '\n',
                       'C', K_DOWN, K_DOWN, '\n', 'C', K_DOWN, K_DOWN, K_DOWN, '\n', END });
    assert(P->sort == SORT_EXT && mingle_dirs == 1 && (cols_mask & (COL_EXT | COL_PERM)) == (COL_EXT | COL_PERM));
    assert(exists(".config/locachy/files.conf"));
    /* Reset mingle_dirs off via 's' menu so folders-first returns to default. */
    run("reset mingle_dirs off and sort by name",
        (const int[]){ 's', K_DOWN, K_DOWN, K_DOWN, K_DOWN, K_DOWN, K_DOWN, K_DOWN, K_DOWN, '\n',
                       KEY('1', KEYS_CTRL | KEYS_ALT), END });
    assert(!mingle_dirs && P->sort == SORT_NAME);

    run("New empty file (N) + undo, Symlink (L) + undo, Copy path (Ctrl+Shift+C), Invert selection (Ctrl+Shift+I)",
        (const int[]){ 'N', 'b', 'l', 'a', 'n', 'k', '.', 't', 'x', 't', '\n', END });
    assert(exists("Documents/blank.txt"));
    run("undo new empty file", (const int[]){ 'u', END });
    assert(!exists("Documents/blank.txt"));

    clip_paste[0] = '\0';
    run("copy path, symlink + undo, duplicate, invert selection",
        (const int[]){ K_DOWN, KEY('c' & 0x1F, KEYS_CTRL | KEYS_SHIFT),
                       'L', '\n', 'u',
                       K_DOWN, KEY('u' & 0x1F, KEYS_CTRL),
                       KEY('i' & 0x1F, KEYS_CTRL | KEYS_SHIFT), END });
    assert(strcmp(clip_paste, at_home("Documents/c.md")) == 0);
    assert(!exists("Documents/c.md.link") && exists("Documents/c (2).md") && marked(P) == P->nlist);
    for (int i = 0; i < P->nlist; i++) P->list[i].marked = 0;

    int places_before = nplaces;
    mk("Documents/subbm");
    put("Documents/subbm/deep.txt", 0);
    go(at_home("Documents/subbm"), NULL);
    run("Ctrl+D bookmark add/remove, F9 side pane toggle, F4 terminal",
        (const int[]){ KEY('d' & 0x1F, KEYS_CTRL), KEY(K_F1 + 8, 0), KEY(K_F1 + 3, 0), END });
    assert(nplaces == places_before + 1 && strcmp(places[nplaces - 1], TRASH_URL) == 0);
    assert(exists(".config/gtk-3.0/bookmarks") && side_hidden == 1 && strcmp(task[0], "/usr/local/bin/term") == 0);
    run("remove bookmark and show side pane again",
        (const int[]){ KEY('d' & 0x1F, KEYS_CTRL), KEY(K_F1 + 8, 0), K_LEFT, END });
    assert(nplaces == places_before && side_hidden == 0 && here("Documents"));

    run("Ctrl+Shift+F recursive Find Files (*.txt) and Alt+Up back to search root",
        (const int[]){ KEY('f' & 0x1F, KEYS_CTRL | KEYS_SHIFT), '*', '.', 't', 'x', 't', '\n', END });
    assert(is_search(P) && P->nlist >= 10);
    run("up from search returns to search root", (const int[]){ KEY(K_UP, KEYS_ALT), END });
    assert(here("Documents"));

    /* Ark's menus, above Properties: with nothing to paste or undo, in the
     * Details view, the menu of a file ends ... Extract (or Compress),
     * Properties, Sort files, Columns, hidden files: five up from the top
     * (it wraps). An archive: Extract; another file: Compress, named as Ark
     * names it. */
#define ARK_ITEM K_UP, K_UP, K_UP, K_UP, K_UP
    put("Documents/a.zip", 0);
    go(at_home("Documents"), "a.zip");
    nclip = 0;
    while (nundo) undo_free(&undos[--nundo]);
    P->view = VIEW_DETAILS;
    struct click ark[] = { { .id = HIT_FILE + index_of("a.zip"), .button = BTN_RIGHT, .clicks = 1 } };
    clicks = ark;
    run("Extract here", (const int[]){ CLICK(0), ARK_ITEM, '\n', '\n', 'q', END });
    assert(ncalled == 5 && strcmp(called[0], "/bin/true") == 0 && strcmp(called[1], "--batch") == 0 &&
           strcmp(called[2], "--autodestination") == 0 && strcmp(called[3], "--autosubfolder") == 0 &&
           strcmp(called[4], at_home("Documents/a.zip")) == 0 && exists("Documents/a.zip"));
    run("Extract and trash archive", (const int[]){ CLICK(0), ARK_ITEM, '\n', K_DOWN, '\n', 'q', END });
    assert(!exists("Documents/a.zip") && strstr(msg, "Trash"));
    go(at_home("Documents"), "c.md");
    while (nundo) undo_free(&undos[--nundo]);
    struct click compress[] = { { .id = HIT_FILE + index_of("c.md"), .button = BTN_RIGHT, .clicks = 1 } };
    clicks = compress;
    run("Compress to \"c.tar.gz\"", (const int[]){ CLICK(0), ARK_ITEM, '\n', '\n', 'q', END });
    assert(ncalled == 5 && strcmp(called[1], "--add-to") == 0 && strcmp(called[2], at_home("Documents/c.tar.gz")) == 0 &&
           strcmp(called[3], "--changetofirstpath") == 0 && strcmp(called[4], at_home("Documents/c.md")) == 0);
    call_status = 2;
    run("Compress to... (cancelled)", (const int[]){ CLICK(0), ARK_ITEM, '\n', K_DOWN, K_DOWN, '\n', 'q', END });
    assert(ncalled == 4 && strcmp(called[1], "--add") == 0 && strstr(msg, "Cancelled"));
    call_status = 0;
#undef ARK_ITEM

    puts("ALL PASSED");
    return 0;
}
