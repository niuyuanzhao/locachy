/*
 * Drive home's file manager (files.c) by its keys over a scratch home: the
 * screen (which keeps what is drawn, as text), the keyboard and the opening
 * of files are test doubles; the files are real. As root it also checks that the operations are the user's (uid
 * 1000) and that the file permissions return to root's after each.
 *   test-files DIR   (DIR: a new directory, made as the scratch home)
 * The folder request is the real FILES_REQUEST: run it in a private mount
 * namespace with a tmpfs on /run/locachy. The pointer: synthetic events on
 * what the drawing registered. Build with base/ui.c, and with CLOUD_LIST
 * set to a built files-cloud-list; the cloud service is a stand-in script.
 */
#define ARCHIVE_PROGRAM "/bin/true" /* there and runnable: its menus are shown */
#define CLOUD_STATUS cloud_status_path
#define CLOUD_CLIENT cloud_client_path
#define SEAT_CLIENT seat_client_path /* a script of the test's: what it was asked */
static char cloud_status_path[256], cloud_client_path[256], seat_client_path[256];
#include "../progs/files.c"
#include "i18n-stub.h"
#include <assert.h>

/* The open-request spool as home reads it: one req-* file per request. */
static int spool_seq;

static void spool_put(const char *text)
{
    char path[PATH_MAX];
    mkdir(OPEN_REQUEST, 0700);
    snprintf(path, sizeof path, "%s/req-%020d-test", OPEN_REQUEST, ++spool_seq);
    FILE *f = fopen(path, "w");
    assert(f && fputs(text, f) >= 0 && fclose(f) == 0);
}

static int spool_count(void)
{
    int n = 0;
    DIR *d = opendir(OPEN_REQUEST);
    for (struct dirent *e; d && (e = readdir(d)) != NULL;) n += strncmp(e->d_name, "req-", 4) == 0;
    if (d) closedir(d);
    return n;
}

static void spool_clear(void)
{
    DIR *d = opendir(OPEN_REQUEST);
    for (struct dirent *e; d && (e = readdir(d)) != NULL;) {
        char path[PATH_MAX];
        if (strncmp(e->d_name, "req-", 4) != 0 && strncmp(e->d_name, ".claim-", 7) != 0) continue;
        snprintf(path, sizeof path, "%s/%s", OPEN_REQUEST, e->d_name);
        unlink(path);
    }
    if (d) closedir(d);
}

/* Interleavings injected into renameat2 (test-files.sh links with --wrap):
 * another writer creates race_target in the gap just before a no-replace
 * rename, or the drive refuses the flag (EINVAL, as a FUSE without rename2;
 * across filesystems the kernel says EXDEV before a drive is asked).
 * Nothing sleeps: the injection is the gap itself. */
static char race_target[PATH_MAX], race_text[64];
static int refuse_noreplace;
int __real_renameat2(int olddir, const char *from, int newdir, const char *to, unsigned int flags);
int __wrap_renameat2(int olddir, const char *from, int newdir, const char *to, unsigned int flags)
{
    if (flags & RENAME_NOREPLACE) {
        if (refuse_noreplace) {
            char up[PATH_MAX];
            struct stat a, b;
            parent_of(to, up, sizeof up);
            errno = lstat(from, &a) == 0 && stat(up, &b) == 0 && a.st_dev != b.st_dev ? EXDEV : EINVAL;
            return -1;
        }
        if (race_target[0] && strcmp(to, race_target) == 0) {
            FILE *f = fopen(to, "we");
            assert(f && fputs(race_text, f) >= 0 && fclose(f) == 0);
            race_target[0] = '\0';
        }
    }
    return __real_renameat2(olddir, from, newdir, to, flags);
}

/* Closes of a regular file with bytes and no name left (--wrap=close): an
 * unfinished copy is removed before it is closed, so a FUSE drive never
 * uploads it (the empty probe of no_rename2 is not counted). */
static int closed_unlinked;
int __real_close(int fd);
int __wrap_close(int fd)
{
    struct stat st;
    if (fstat(fd, &st) == 0 && S_ISREG(st.st_mode) && st.st_nlink == 0 && st.st_size > 0) closed_unlinked++;
    return __real_close(fd);
}

static void write_file(const char *path, const char *text)
{
    FILE *f = fopen(path, "we");
    assert(f && fputs(text, f) >= 0 && fclose(f) == 0);
}

/* The bytes of path (NUL-terminated in buf), or NULL when it cannot be read. */
static char *read_file(const char *path, char *buf, size_t size)
{
    FILE *f = fopen(path, "re");
    if (!f) return NULL;
    size_t n = fread(buf, 1, size - 1, f);
    buf[n] = '\0';
    fclose(f);
    return buf;
}

static int same_text(const char *path, const char *want)
{
    char buf[256];
    const char *got = read_file(path, buf, sizeof buf);
    return got && strcmp(got, want) == 0;
}

/* How many entries of dir start with prefix. */
static int entries_with(const char *dir, const char *prefix)
{
    int n = 0;
    DIR *d = opendir(dir);
    for (struct dirent *e; d && (e = readdir(d)) != NULL;) n += strncmp(e->d_name, prefix, strlen(prefix)) == 0;
    if (d) closedir(d);
    return n;
}

#define END     0
#define TIMEOUT (-1) /* key_poll_timeout times out */
#define REQUEST (-2) /* a container asks for the folder in request; a timeout */
#define OPEN    (-3) /* open_in_home's request: request is the file's text; a timeout */
#define WAIT    (-4) /* a timeout after 50 ms: a cloud worker has time to end */
#define HOTPLUG (-5) /* one output removed while a drag/pull is in progress */
#define AFTER_HOTPLUG (-6) /* verify the real browse K_SHOWN path */
#define IDLE    (-7) /* nothing typed, for as long as a copy runs (then a timeout) */
#define TERM    (-8) /* SIGTERM to this process (home's Stop); a timeout */
#define SCROLL(i) (-200 - (i)) /* scrolls[i], as K_POINTER */
#define KEY(k, m) (-1000000 - ((m) << 16) - (k)) /* key k with modifiers m */
#define CTRL_KEY(c) KEY((c) & 0x1F, KEYS_CTRL)    /* Ctrl with a letter: its control code */
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
static const struct pointer_event *scrolls;
static struct pointer_event mouse;
static int mods, menu_col, menu_row, px_fills, scroll_opted;
static char clip_paste[256];
const struct pointer_event *pointer_last(void) { return &mouse; }
int pointer_scroll_events(int on)
{
    int was = scroll_opted;
    scroll_opted = on;
    return was;
}
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
/* What is drawn: a character per cell (a wide one in its first, 0 in its
 * second), as screen.c draws it on ncols columns (at most 100): a fill
 * blanks, text is cut at the edge. And the bar's title. */
static int ncols = 100, native_fixture, native_on, native_output, native_count = 2;
static uint32_t drawn[30][100], drawn_bg[30][100];
static uint32_t output_drawn[2][30][100], output_bg[2][30][100];
static int native_cols[] = { 76, 100 }, native_rows[] = { 27, 30 };
static int native_cell_w[] = { 8, 12 }, native_cell_h[] = { 16, 24 };
static const char *native_names[] = { "eDP-1", "HDMI-A-1" };
static char bar_title[64];
int screen_cols(void) { return native_on ? native_cols[native_output] : ncols; }
int screen_rows(void) { return native_on ? native_rows[native_output] : 30; }
void screen_cells(int *w, int *h, int *t)
{
    *w = native_on ? native_cell_w[native_output] : 8;
    *h = native_on ? native_cell_h[native_output] : 16;
    *t = 0;
}
void screen_pixels(int *w, int *h)
{
    int cw, ch, top;
    screen_cells(&cw, &ch, &top);
    *w = screen_cols() * cw;
    *h = screen_rows() * ch + top;
}
int screen_extended(int on)
{
    if (!native_fixture) return -1;
    int changed = native_on != !!on;
    native_on = !!on;
    if (native_output >= native_count) native_output = 0;
    return changed;
}
int screen_is_extended(void) { return native_on; }
int screen_output_current(void) { return native_on ? native_output : 0; }
int screen_output_count(void) { return native_on ? native_count : 1; }
int screen_output_get(int i, struct screen_output *out)
{
    if (i < 0 || i >= screen_output_count()) return -1;
    memset(out, 0, sizeof *out);
    snprintf(out->name, sizeof out->name, "%s", native_names[i]);
    out->x = i ? 608 : 0;
    out->pixel_width = native_cols[i] * native_cell_w[i];
    out->pixel_height = native_rows[i] * native_cell_h[i];
    out->scale = i ? 1.5 : 1;
    out->width = out->pixel_width / out->scale;
    out->height = out->pixel_height / out->scale;
    return 0;
}
int screen_output_select(int i)
{
    if (i < 0 || i >= screen_output_count()) return -1;
    int old = native_output;
    if (native_on) {
        memcpy(output_drawn[old], drawn, sizeof drawn);
        memcpy(output_bg[old], drawn_bg, sizeof drawn_bg);
        native_output = i;
        memcpy(drawn, output_drawn[i], sizeof drawn);
        memcpy(drawn_bg, output_bg[i], sizeof drawn_bg);
    }
    return old;
}
int screen_output_at(int x, int y, int *lx, int *ly)
{
    for (int i = 0; i < native_count; i++) {
        struct screen_output o;
        screen_output_get(i, &o);
        if (x < o.x || y < 0 || x >= o.x + o.width || y >= o.height) continue;
        if (lx) *lx = (x - o.x) * o.scale;
        if (ly) *ly = y * o.scale;
        return i;
    }
    return -1;
}
int screen_char_cells(uint32_t cp) { return cp >= 0x4E00 && cp <= 0x9FFF ? 2 : 1; }
void screen_fill(int c, int r, int w, int h, uint32_t rgb)
{
    for (int y = r; y < r + h; y++)
        for (int x = c; x < c + w; x++)
            if (y >= 0 && y < screen_rows() && x >= 0 && x < screen_cols()) { drawn[y][x] = ' '; drawn_bg[y][x] = rgb; }
}
void screen_fill_px(int x, int y, int w, int h, uint32_t rgb)
{
    int width, height;
    screen_pixels(&width, &height);
    if (x == 0 && y == 0 && w == width && h == height) screen_fill(0, 0, screen_cols(), screen_rows(), rgb);
    px_fills++;
}
void screen_text(int c, int r, const char *s, uint32_t fg, uint32_t bg)
{
    (void)fg;
    for (size_t i = 0; r >= 0 && r < screen_rows() && s[i] && c < screen_cols(); i = utf_next(s, i), c++) {
        uint32_t cp = utf_at(s, i, utf_next(s, i));
        if (c >= 0) { drawn[r][c] = cp; drawn_bg[r][c] = bg; }
        if (screen_char_cells(cp) == 2 && c + 1 < screen_cols()) { drawn[r][++c] = 0; drawn_bg[r][c] = bg; }
    }
}
void screen_flush(void)
{
    if (native_on) {
        memcpy(output_drawn[native_output], drawn, sizeof drawn);
        memcpy(output_bg[native_output], drawn_bg, sizeof drawn_bg);
    }
}
void screen_bar(const char *title, const char *ime)
{
    (void)ime;
    snprintf(bar_title, sizeof bar_title, "%s", title);
}

/* Row r as drawn from column c, in UTF-8, without the blanks at its end. */
static const char *drawn_row(int r, int c)
{
    static char line[100 * 3 + 1];
    size_t n = 0;
    for (; c < ncols; c++) {
        uint32_t cp = drawn[r][c];
        if (cp == 0) {
            continue;
        } else if (cp < 0x80) {
            line[n++] = (char)cp;
        } else if (cp < 0x800) {
            line[n++] = (char)(0xC0 | cp >> 6);
            line[n++] = (char)(0x80 | (cp & 63));
        } else {
            line[n++] = (char)(0xE0 | cp >> 12);
            line[n++] = (char)(0x80 | (cp >> 6 & 63));
            line[n++] = (char)(0x80 | (cp & 63));
        }
    }
    while (n && line[n - 1] == ' ') n--;
    line[n] = '\0';
    return line;
}
int rc_set(const char *name, const char *value) { (void)name; (void)value; return 0; }
int rc_read(void (*fn)(const char *, const char *, void *), void *arg)
{ (void)fn; (void)arg; return 0; }

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

/* While a copy runs: how often it looked at the keys, whether its hidden
 * temporary was there, and whether anything was ever under watch_final. */
static int copy_polls, saw_temp, saw_final, kept_closes;
static char watch_final[PATH_MAX];
void key_close_kept(void) { kept_closes++; }

int key_poll_timeout(int fd, int timeout)
{
    (void)fd;
    (void)timeout;
    mods = 0;
    int key = script[step];
    if (key == END) {
        assert(++extra < 50); /* Ctrl+Q does not get out: stuck */
        mods = KEYS_CTRL;
        return 'q' & 0x1F; /* the script's end: quit, as Ctrl+Q does */
    }
    if (key == IDLE && copying.temp[0]) {
        struct stat st;
        copy_polls++;
        saw_temp |= lstat(copying.temp, &st) == 0;
        saw_final |= watch_final[0] && lstat(watch_final, &st) == 0;
        return 0;
    }
    step++;
    if (key == IDLE) {
        return 0;
    }
    if (key == TERM) {
        kill(getpid(), SIGTERM);
        return 0;
    }
    if (key <= -1000000) {
        int raw = -1000000 - key;
        mods = (raw >> 16) & 0xF;
        return raw & 0xFFFF;
    }
    if (key <= SCROLL(0) && key > -1000000) {
        mouse = scrolls[SCROLL(0) - key]; /* over the pane's empty space */
        hit_cell(HIT_AREA, &mouse.col, &mouse.row);
        mods = 0;
        return K_POINTER;
    }
    if (key <= CLICK(0)) {
        point(&clicks[CLICK(0) - key]);
        return K_POINTER;
    }
    if (key == HOTPLUG) {
        native_count = 1;
        drag.mode = DRAG_BAND;
        drag.base = calloc(1, 1);
        pull.on = 1;
        return K_SHOWN;
    }
    if (key == AFTER_HOTPLUG) {
        assert(drag.mode == DRAG_NONE && !pull.on && files_output == 0 && native_output == 0);
        mods = KEYS_CTRL;
        return 'q' & 0x1F;
    }
    if (key == WAIT) {
        usleep(50000);
        return 0;
    }
    if (key == REQUEST || key == OPEN) {
        if (key == OPEN) {
            spool_put(request);
        } else {
            FILE *f = fopen(FILES_REQUEST, "w");
            assert(f);
            fprintf(f, "%s\n", request);
            fclose(f);
        }
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

/* "program:viewer" is a framebuffer program: a task of its own (base_open);
 * so are "program:view", the document viewer as installed (a window of its
 * own from a window), and "program:edit" (from home only). */
static char task[2][PATH_MAX];
static int tasks;
int open_program(const char *name, const char *target, char *spec, size_t size, char *argv[], int max)
{
    const char *binary = !strcmp(name, "program:viewer") ? "/usr/local/bin/viewer" :
                         !strcmp(name, "program:view") ? "/usr/local/bin/view" :
                         !strcmp(name, "program:edit") ? "/usr/local/bin/edit" : NULL;
    if (!binary || max < 3) {
        return 0;
    }
    snprintf(spec, size, "%s", binary);
    argv[0] = spec;
    argv[1] = (char *)target;
    argv[2] = NULL;
    return 2;
}

/* Process 1's answer: home had one screen, and has it again; or "shown"
 * (several: home kept the others). */
static const char *open_answer = "back viewer";
void base_open(char *const argv[], char *why, size_t size)
{
    tasks++;
    snprintf(task[0], sizeof task[0], "%s", argv[0]);
    snprintf(task[1], sizeof task[1], "%s", argv[1] ? argv[1] : "");
    snprintf(why, size, "%s", open_answer);
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

/* The Wayland window's data device (wayland-data.c): off unless a scenario
 * turns it on; a drag records its text/uri-list and press serial, one with
 * a document-portal key also the key and how it is told its end. */
static int wl_on, wl_starts, wl_key_starts;
static char wl_uris[8192], wl_key[8192];
static uint32_t wl_serial;
static void (*wl_ended)(int finished, void *data);
static void *wl_ended_data;
int wl_data_active(void) { return wl_on; }
uint32_t wl_data_press_serial(void) { return 4242; }
int wl_data_drag_start(const char *uris, size_t size, uint32_t serial)
{
    wl_starts++;
    wl_serial = serial;
    snprintf(wl_uris, sizeof wl_uris, "%.*s", (int)size, uris);
    return 0;
}
int wl_data_drag_start_key(const char *uris, size_t size, const char *key, uint32_t serial,
                           void (*ended)(int finished, void *data), void *data)
{
    wl_key_starts++;
    wl_serial = serial;
    snprintf(wl_uris, sizeof wl_uris, "%.*s", (int)size, uris);
    snprintf(wl_key, sizeof wl_key, "%s", key);
    wl_ended = ended;
    wl_ended_data = data;
    return 0;
}

/* A stand-in document service on DOCS_CONTROL (docs-proto.h) for one
 * connection: each begin's names, and the paths its descriptors have, go to
 * DOCS_DIR/log with the end; a begin is answered with answer, or with the
 * documents' paths as the service gives them (session 7, document dN). */
static pid_t fake_docs(const char *answer)
{
    struct sockaddr_un a = { .sun_family = AF_UNIX, .sun_path = DOCS_CONTROL };
    mkdir("/run/locachy", 01777);
    mkdir(DOCS_DIR, 0755);
    unlink(DOCS_CONTROL);
    unlink(DOCS_DIR "/log");
    int s = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0);
    assert(s >= 0 && bind(s, (struct sockaddr *)&a, sizeof a) == 0 && listen(s, 1) == 0);
    pid_t pid = fork();
    assert(pid >= 0);
    if (pid > 0) {
        close(s);
        return pid;
    }
    int c = accept(s, NULL, NULL), docs = 0;
    FILE *log = fopen(DOCS_DIR "/log", "w");
    for (;;) {
        char text[DOCS_MSG_MAX + 1], control[CMSG_SPACE(sizeof(int) * DOCS_FDS_MAX)];
        struct iovec iov = { .iov_base = text, .iov_len = DOCS_MSG_MAX };
        struct msghdr m = { .msg_iov = &iov, .msg_iovlen = 1, .msg_control = control, .msg_controllen = sizeof control };
        ssize_t n = recvmsg(c, &m, 0);
        if (n <= 0) break;
        text[n] = '\0';
        int fds[DOCS_FDS_MAX], nfds = 0;
        struct cmsghdr *h = CMSG_FIRSTHDR(&m);
        if (h && h->cmsg_type == SCM_RIGHTS) {
            nfds = (int)((h->cmsg_len - CMSG_LEN(0)) / sizeof(int));
            memcpy(fds, CMSG_DATA(h), sizeof(int) * (size_t)nfds);
        }
        char reply[DOCS_MSG_MAX] = "ok 7", *save = NULL;
        size_t len = strlen(reply);
        fprintf(log, "%s\n", strtok_r(text, "\n", &save));
        int k = 0;
        for (char *name = strtok_r(NULL, "\n", &save); name; name = strtok_r(NULL, "\n", &save), k++) {
            char at[64], path[PATH_MAX] = "?";
            snprintf(at, sizeof at, "/proc/self/fd/%d", k < nfds ? fds[k] : -1);
            ssize_t r = readlink(at, path, sizeof path - 1);
            if (r > 0) path[r] = '\0';
            int flags = k < nfds ? fcntl(fds[k], F_GETFL) : -1;
            fprintf(log, "name %s %s%s\n", name, path, flags >= 0 && (flags & O_PATH) ? " O_PATH" : "");
            len += (size_t)snprintf(reply + len, sizeof reply - len, "\n/run/user/1000/doc/d%d/%s", ++docs, name);
        }
        for (int i = 0; i < nfds; i++) close(fds[i]);
        fflush(log);
        if (strncmp(text, "begin", 5) == 0) {
            const char *out = answer ? answer : reply;
            send(c, out, strlen(out), MSG_NOSIGNAL);
        }
    }
    fclose(log);
    _exit(0);
}

/* The stand-in's log, once its connection ended. */
static const char *fake_docs_log(pid_t pid)
{
    static char text[8192];
    assert(waitpid(pid, NULL, 0) == pid);
    FILE *f = fopen(DOCS_DIR "/log", "r");
    size_t n = f ? fread(text, 1, sizeof text - 1, f) : 0;
    text[n] = '\0';
    if (f) fclose(f);
    return text;
}

/* A stand-in top bar on its open socket (BAR_OPEN_SOCKET) for one request:
 * what it was asked goes to BAR_LOG, answer is its reply. */
#define BAR_LOG "/run/locachy/open/log"
static pid_t fake_bar(const char *answer)
{
    struct sockaddr_un a = { .sun_family = AF_UNIX, .sun_path = BAR_OPEN_SOCKET };
    mkdir("/run/locachy", 01777);
    mkdir("/run/locachy/open", 0755);
    unlink(BAR_OPEN_SOCKET);
    unlink(BAR_LOG);
    int s = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0);
    assert(s >= 0 && bind(s, (struct sockaddr *)&a, sizeof a) == 0 && listen(s, 1) == 0);
    pid_t pid = fork();
    assert(pid >= 0);
    if (pid > 0) {
        close(s);
        return pid;
    }
    char request[PATH_MAX + 128];
    int c = accept(s, NULL, NULL);
    ssize_t n = c >= 0 ? recv(c, request, sizeof request - 1, 0) : -1;
    FILE *log = fopen(BAR_LOG, "w");
    if (log) {
        fprintf(log, "%.*s", n > 0 ? (int)n : 0, request);
        fclose(log);
    }
    if (c >= 0) send(c, answer, strlen(answer), MSG_NOSIGNAL);
    _exit(0);
}

/* A file's text (the stand-ins' logs), "" when there is none. */
static const char *text_of(const char *path)
{
    static char text[4096];
    FILE *f = fopen(path, "r");
    size_t n = f ? fread(text, 1, sizeof text - 1, f) : 0;
    text[n] = '\0';
    if (f) fclose(f);
    return text;
}

static const char *fake_bar_log(pid_t pid)
{
    assert(waitpid(pid, NULL, 0) == pid);
    return text_of(BAR_LOG);
}

/* The copies' hidden temporaries in a folder. */
static int temps_in(const char *dir)
{
    int n = 0;
    DIR *d = opendir(dir);
    struct dirent *e;
    while (d && (e = readdir(d)) != NULL) n += strncmp(e->d_name, COPY_TEMP, strlen(COPY_TEMP)) == 0;
    if (d) closedir(d);
    return n;
}

/* A file of size bytes at path. */
static void put_bytes(const char *path, size_t size)
{
    static char block[65536];
    memset(block, 'x', sizeof block);
    FILE *f = fopen(path, "w");
    assert(f);
    for (size_t done = 0; done < size; done += sizeof block) {
        assert(fwrite(block, 1, size - done < sizeof block ? size - done : sizeof block, f) > 0);
    }
    assert(fclose(f) == 0);
}

static long long size_of(const char *path)
{
    struct stat st;
    return lstat(path, &st) == 0 ? (long long)st.st_size : -1;
}

/* transfer (paste, a drop) of from into dir with these keys: they must all
 * be used. */
static void copy_with(const char *what, const char *from, const char *dir, int cut, const int *keys)
{
    char path[PATH_MAX];
    char *list[] = { path };
    printf("== %s\n", what);
    snprintf(path, sizeof path, "%s", from);
    script = keys;
    step = extra = 0;
    error = 0;
    copy_polls = saw_temp = saw_final = 0;
    transfer(list, 1, dir, cut);
    if (script[step] == IDLE) step++; /* the copy ended while nothing was typed */
    assert(script[step] == END && extra == 0);
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
                            strcmp(type, "application/pdf") == 0 || strcmp(type, "inode/directory") == 0 ? viewers :
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

int base_app_open_own_tor(const char *name, const char *target, char *why, size_t size)
{
    return base_app_open(name, target, why, size);
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
    copy_tick_interval = 1e9; /* copies look at the keys only where a scenario says */
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

    /* A folder: o offers what lists inode/directory (view's .desktop does),
     * with no A (folders open here whatever a rule says: open_set_rule's
     * double would refuse it); Enter still goes in. */
    run("open with on a folder: the viewer offered, no rule; Enter goes in, Left back",
        (const int[]){ K_BOL, 'o', 'a', '\n', '\n', K_LEFT, END });
    assert(tasks == 1 && strcmp(task[1], at_home("Documents/sub")) == 0 && strcmp(rule, "one") == 0 && opens == 2);
    assert(here("Documents") && strcmp(P->list[P->sel].name, "sub") == 0);
    tasks = 0;

    char kept[PATH_MAX];
    snprintf(kept, sizeof kept, "%s", P->cwd);
    run("the home key: back to home's list at once, the place kept", (const int[]){ K_ACT_HOME, END });
    assert(extra == 0 && strcmp(P->cwd, kept) == 0);

    /* What it drew (Documents: sub/ a.txt b.txt, sub selected): the title, the
     * toolbar, the column headers, the status and the buttons, nothing more. */
    static const char status_start[] = " sub \xe2\x80\x94 1 item, inode/directory, ";
    char toolbar[PATH_MAX + 64];
    snprintf(toolbar, sizeof toolbar, " < > ^ %-55s[Refresh]  Icons   Compact   Details", kept); /* the path: 54 columns */
    assert(strcmp(bar_title, "files") == 0 && (strlen(kept) > 54 || strcmp(drawn_row(1, 0), toolbar) == 0));
    assert(strcmp(drawn_row(2, SIDE), "  Name ^                          Size  Modified          Type") == 0);
    assert(strncmp(drawn_row(27, 0), status_start, strlen(status_start)) == 0);
    assert(strstr(drawn_row(27, 0), "  3 items (1 hidden) | "));
    /* The buttons name the table keys. Ctrl+Shift+N: new folder and
     * Ctrl+Z: undo do not fit in 100 columns (F1 and the menu list them). */
    assert(strcmp(drawn_row(28, 0), " [Enter: open] [o: open with] [Ctrl+C: copy] [Ctrl+X: cut] [Ctrl+V: paste] "
                                    "[F2: rename] [Del: trash]") == 0);
    assert(strcmp(drawn_row(29, 0), " [/: filter] [.: hidden] [s: sort] [i: props] [F3: split] [w: wallpaper] "
                                    "[e: eject] [Esc: back]") == 0);

    /* 68 columns (1366 pixels at scale 2): what does not fit is left out,
     * never the way back. */
    struct click back[] = { { .id = HIT_KEY + 27, .button = BTN_LEFT, .clicks = 1 } };
    clicks = back;
    ncols = 68;
    run("a narrow screen: Esc: back is still shown, and clicking it goes back", (const int[]){ CLICK(0), END });
    assert(extra == 0 && strcmp(drawn_row(29, 0), " [/: filter] [.: hidden] [s: sort] [i: props] [e: eject] "
                                                  "[Esc: back]") == 0);
    ncols = 100;

    run("open: the rule, no chooser", (const int[]){ K_DOWN, K_DOWN, '\n', END });
    assert(opens == 3 && strcmp(opened_name, "one") == 0);
    assert(strcmp(opened_target, at_home("Documents/b.txt")) == 0);

    run("open: not in a shared folder", (const int[]){ K_LEFT, K_EOL, '\n', END });
    assert(opens == 3 && error && strstr(msg, "containers cannot see it") && here(""));

    struct stat src_st, copy_st;
    assert(stat(at_home("Documents/a.txt"), &src_st) == 0 && chmod(at_home("Documents/a.txt"), 06755) == 0);
    run("copy, paste twice", (const int[]){ '\n', K_DOWN, CTRL_KEY('c'), K_LEFT, K_DOWN, '\n', CTRL_KEY('v'), CTRL_KEY('v'), END });
    assert(here("Downloads") && exists("Downloads/a.txt") && exists("Downloads/a (2).txt"));
    assert(exists("Documents/a.txt") && !error);
    assert(!is_root || (owner("Downloads/a.txt") == USER && owner("Downloads/a (2).txt") == USER));
    /* As cp without -p: no setuid or setgid bit with the copy, the rest kept. */
    assert(stat(at_home("Downloads/a.txt"), &copy_st) == 0 && (copy_st.st_mode & 07000) == 0 &&
           (copy_st.st_mode & 0100));
    assert(chmod(at_home("Documents/a.txt"), src_st.st_mode & 07777) == 0);

    /* Downloads: a (2).txt, a.txt */
    run("cut, paste: moved", (const int[]){ CTRL_KEY('x'), K_LEFT, K_BOL, '\n', CTRL_KEY('v'), END });
    assert(exists("Documents/a (2).txt") && !exists("Downloads/a (2).txt") && nclip == 0);

    /* Documents: sub/ a (2).txt a.txt b.txt */
    run("mark two, delete permanently (Shift+Delete): no, then yes",
        (const int[]){ K_DOWN, ' ', K_DOWN, ' ', KEY(K_DEL, KEYS_SHIFT), 'n', KEY(K_DEL, KEYS_SHIFT), 'y', END });
    assert(!exists("Documents/a (2).txt") && !exists("Documents/b.txt") && exists("Documents/a.txt"));

    run("rename, new folder",
        (const int[]){ K_DOWN, KEY(K_F1 + 1, 0), '\b', '\b', '\b', '\b', '\b', 'c', '.', 'm', 'd', '\n',
                       KEY('n' & 0x1F, KEYS_CTRL | KEYS_SHIFT), 'n', 'e', 'w', '\n', END });
    assert(exists("Documents/c.md") && !exists("Documents/a.txt") && exists("Documents/new"));
    assert(!is_root || owner("Documents/new") == USER);

    /* Documents: new/ sub/ c.md */
    run("a folder is not moved into itself", (const int[]){ CTRL_KEY('x'), '\n', CTRL_KEY('v'), END });
    assert(here("Documents/new") && exists("Documents/new") && error);

    run("delete a folder with what is in it", (const int[]){ K_LEFT, K_DOWN, KEY(K_DEL, KEYS_SHIFT), 'y', END });
    assert(!exists("Documents/sub") && here("Documents"));

    if (is_root) {
        /* Root's: the user cannot delete it, and root's permissions come back. */
        assert(mkdir(at_home("Documents/locked"), 0755) == 0);
        FILE *f = fopen(at_home("Documents/locked/f"), "w");
        assert(f);
        fclose(f);
        run("the user's permissions: root's files stay", (const int[]){ KEY(K_DEL, KEYS_SHIFT), 'y', END });
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
    run("filter (the home key leaves it as it is)", (const int[]){ '/', 'c', '\n', K_ACT_HOME, END });
    assert(P->nlist == 1 && strcmp(P->list[0].name, "c.md") == 0);
    run("hidden files", (const int[]){ '.', 'q', END });
    assert(P->nlist == 3);
    run("hidden files off", (const int[]){ '.', 'q', END });
    assert(P->nlist == 2);

    run("places", (const int[]){ '\t', K_DOWN, K_DOWN, '\n', 'q', END });
    assert(here("Downloads"));

    /* The keys every program has (key_action): q is Esc in the list, Ctrl+Q
     * quits (inside home: back to its list), Alt+q and Super+q are not q,
     * F1's help and Shift+F10's menu close with Esc. */
    run("q clears the marks first, then goes back to home's list", (const int[]){ ' ', 'q', 'q', END });
    assert(extra == 0 && marked(P) == 0 && !files_close_asked());
    run("Alt+q and Super+q are not q; F1's help and Shift+F10's menu close with Esc",
        (const int[]){ KEY('q', KEYS_ALT), KEY('q', KEYS_META), KEY(K_F1, 0), 27, KEY(K_F1 + 9, KEYS_SHIFT), 27, END });
    assert(extra == 1 && files_close_asked());
    run("Ctrl+Q leaves at once, the marks kept", (const int[]){ ' ', KEY('q' & 0x1F, KEYS_CTRL), END });
    assert(extra == 0 && files_close_asked() && marked(P) == 1);
    for (int i = 0; i < P->nlist; i++) P->list[i].marked = 0;
    standalone = 1;
    run("files on its own: Esc and q at the top and the home key change nothing; Ctrl+Q leaves",
        (const int[]){ 27, 'q', K_ACT_HOME, KEY('q' & 0x1F, KEYS_CTRL), END });
    assert(extra == 0 && files_close_asked() && here("Downloads"));
    assert(strstr(drawn_row(29, 0), "[Ctrl+Q: quit]"));
    standalone = 0;

    /* Choosing (settings' wallpaper): only a file of the type; nothing changes. */
    char chosen[PATH_MAX] = "";
    assert(pick("choose: not this one, nor d or x, a folder is gone into, then a text", "text/",
                at_home("Documents"), "c.md",
                (const int[]){ '\n', K_DEL, CTRL_KEY('x'), K_BOL, '\n', K_LEFT, '\t', K_DOWN, K_DOWN, '\n', K_BOL, '\n', END },
                chosen, sizeof chosen) == 1);
    assert(strcmp(chosen, at_home("Downloads/a.txt")) == 0);
    assert(exists("Documents/c.md") && exists("Documents/new") && nclip == 0);
    chosen[0] = '\0';
    assert(pick("choose: Esc, nothing chosen", "text/", at_home("Downloads"), NULL, (const int[]){ 27, END },
                chosen, sizeof chosen) == 0 && !chosen[0]);
    assert(pick("choose: the home key leaves the choice open (Ctrl+Q ends it)", "text/", at_home("Downloads"),
                NULL, (const int[]){ K_ACT_HOME, END }, chosen, sizeof chosen) == 0 && extra == 1);

    /* Choosing a folder (packages: a container's directories): o on it. */
    assert(pick("choose a folder: o on a file is refused, nor d; Enter goes in, o chooses",
                "inode/directory", at_home("Documents"), "c.md",
                (const int[]){ 'o', K_DEL, K_BOL, '\n', K_LEFT, 'o', END }, chosen, sizeof chosen) == 1);
    assert(strcmp(chosen, at_home("Documents/new")) == 0 && exists("Documents/new") && exists("Documents/c.md"));

    /* The pointer. Documents: new/ c.md; home: Documents/ Downloads/ notes.txt. */
    nclip = 0;
    struct click tour[] = {
        { .id = HIT_PLACE + 0, .button = BTN_LEFT, .clicks = 1 },
        { .id = HIT_FILE + 0, .button = BTN_LEFT, .clicks = 2 },
        { .id = HIT_PATH + (int)strlen(scratch), .button = BTN_LEFT, .clicks = 1 },
        { .id = HIT_FILE + 0, .button = BTN_LEFT, .clicks = 2 },
        { .id = HIT_FILE + 1, .button = BTN_RIGHT, .clicks = 1 },
        { .id = HIT_KEY + MODS(KEYS_CTRL) + ('v' & 0x1F), .button = BTN_LEFT, .clicks = 1 },
        { .id = AT, .col = SIDE + 10, .row = 12, .button = BTN_RIGHT, .clicks = 1 },
        { .id = AT, .col = SIDE + 10, .row = 12, .button = BTN_RIGHT, .release = 1 },
        { .id = MENU(0) },
        { .id = MENU(0), .button = BTN_LEFT, .clicks = 1 },
        { .id = MENU(0), .button = BTN_LEFT, .release = 1 },
        { .id = HIT_KEY + 27, .button = BTN_LEFT, .clicks = 1 },
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
    run("pointer: Shift marks a range, Ctrl one; delete answered by buttons (a motion is no answer); the wheel keeps selection",
        (const int[]){ CLICK(0), CLICK(1), CLICK(2), CLICK(3), KEY(K_DEL, KEYS_SHIFT), CLICK(4), CLICK(5), KEY(K_DEL, KEYS_SHIFT), CLICK(6),
                       CLICK(7), 'q', END });
    assert(!exists("Documents/p") && !exists("Documents/c (2).md"));
    assert(exists("Documents/new") && exists("Documents/c.md") && P->sel == 0 && P->top == 0);

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

    /* One of home's programs to run by name ("program:NAME", no target: the
     * top bar's launcher) is home's, not the file manager's: it goes back to
     * home's list, the request left whole for home to take. */
    snprintf(request, sizeof request, "program:settings\n\n");
    run("a program for home to run: back to home's list, the request left for it", (const int[]){ OPEN, END });
    char asked[OPEN_NAME];
    assert(tasks == 2 && opens == 8 && !files_request() && spool_count() == 1);
    assert(files_program_request(asked, sizeof asked, 0) == 1 && strcmp(asked, "settings") == 0);
    assert(files_program_request(asked, sizeof asked, 1) == 1 && spool_count() == 0);
    assert(files_program_request(asked, sizeof asked, 1) == 0);
    spool_put("program:view\n/x/y.pdf\n");
    assert(files_program_request(asked, sizeof asked, 1) == 0 && files_request() && spool_count() == 1);
    spool_clear();

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
        (const int[]){ K_EOL, K_DEL, END });
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
        (const int[]){ '\t', K_DOWN, '\n', K_DOWN, K_DEL, '\t', K_DOWN, K_DOWN, K_DOWN, '\n', 'e', 'y', END });
    assert(strcmp(P->cwd, TRASH_URL) == 0 && P->nlist == 0 && !exists(".local/share/Trash/files/ doomed.txt"));

    /* Undo for New Folder, Rename, Cut+Paste Move, and Copy+Paste. */
    run("undo new folder, rename, move, copy",
        (const int[]){ '\t', K_DOWN, '\n',
                       KEY('n' & 0x1F, KEYS_CTRL | KEYS_SHIFT), 'u', 'd', 'i', 'r', '\n', CTRL_KEY('z'),
                       K_DOWN, KEY(K_F1 + 1, 0), '\b', '\b', '\b', 'b', 'a', 'k', '\n', CTRL_KEY('z'),
                       KEY('c' & 0x1F, KEYS_CTRL), KEY('v' & 0x1F, KEYS_CTRL), CTRL_KEY('z'), 'y', END });
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
        (const int[]){ KEY(K_CTRL_0 + 1, KEYS_CTRL), K_RIGHT, K_RIGHT, K_DOWN, K_LEFT,
                       KEY('+', KEYS_CTRL), KEY('-', KEYS_CTRL), KEY(K_CTRL_0, KEYS_CTRL),
                       KEY(K_CTRL_0 + 2, KEYS_CTRL), K_DOWN, KEY(K_CTRL_0 + 3, KEYS_CTRL), END });
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

    /* Dragging out of the window: shared files as a copy-only text/uri-list
     * with the real press serial; a private file of the home only as a
     * document of the document service (all or nothing); nothing else. */
    put("Documents/a b 中.txt", 0);
    put("secret.txt", 0);
    assert(symlink("../secret.txt", at_home("Documents/to-secret")) == 0);
    assert(symlink("/etc/passwd", at_home("Documents/to-etc")) == 0);
    run("reload Documents", (const int[]){ KEY(K_F1 + 4, 0), END });
    char expect[PATH_MAX + 64];
    snprintf(expect, sizeof expect, "file://%s/Documents/a%%20b%%20%%E4%%B8%%AD.txt\r\n", scratch);
    struct click out[] = {
        { .id = HIT_FILE + index_of("a b 中.txt"), .button = BTN_LEFT, .clicks = 1 },
        { .id = AT, .col = -1, .row = 6 }, /* the pointer leaves the window, the button held */
        { .id = AT, .col = -1, .row = 6, .button = BTN_LEFT, .release = 1 },
        { .id = HIT_FILE + index_of("to-secret"), .button = BTN_LEFT, .clicks = 1 },
        { .id = HIT_FILE + index_of("to-etc"), .button = BTN_LEFT, .clicks = 1 },
    };
    clicks = out;
    wl_starts = 0;
    run("direct screen: an item dragged past the edge stays an internal drag",
        (const int[]){ CLICK(0), CLICK(1), CLICK(2), END });
    assert(wl_starts == 0 && drag.mode == DRAG_NONE);
    wl_on = 1;
    run("Wayland window: a shared file dragged out is a copy-only text/uri-list drag",
        (const int[]){ CLICK(0), CLICK(1), CLICK(2), END });
    assert(wl_starts == 1 && wl_key_starts == 0 && wl_serial == 4242 && strcmp(wl_uris, expect) == 0 &&
           drag.mode == DRAG_NONE);
    assert(exists("Documents/a b 中.txt"));
    puts("PASS a shared file dragged out of a window: one percent-encoded file URI, the press serial, a copy");
    error = 0;
    run("Wayland window: a link out of the home is refused, nothing starts",
        (const int[]){ CLICK(4), CLICK(1), CLICK(2), END });
    assert(wl_starts == 1 && wl_key_starts == 0 && error && strstr(msg, "home folder"));
    puts("PASS a file outside the home (here through a symbolic link) never leaves as a drag");
    error = 0;
    run("Wayland window: a private file without the document service is refused, nothing starts",
        (const int[]){ CLICK(3), CLICK(1), CLICK(2), END });
    assert(wl_starts == 1 && wl_key_starts == 0 && error && strstr(msg, "not running") &&
           strstr(msg, "shared folders") && exists("secret.txt"));
    puts("PASS a private file without the document service never leaves as a drag, and the message says why");
    error = 0;
    pid_t svc = fake_docs("error the portal refused it");
    run("Wayland window: a private file the document service refuses is refused, nothing starts",
        (const int[]){ CLICK(3), CLICK(1), CLICK(2), END });
    const char *svc_log = fake_docs_log(svc);
    assert(wl_starts == 1 && wl_key_starts == 0 && error && strstr(msg, "document service") &&
           docs_drag.fd < 0 && strstr(svc_log, "begin\nname secret.txt ") && !strstr(svc_log, "end "));
    puts("PASS a private file the document service refuses: nothing starts, the connection closes (all or nothing)");
    error = 0;
    svc = fake_docs(NULL);
    run("Wayland window: a private file dragged out is a document of the document service",
        (const int[]){ CLICK(3), CLICK(1), CLICK(2), END });
    char doc_uri[] = "file:///run/user/1000/doc/d1/secret.txt\r\n", decoded[512];
    long n = docs_key_decode(wl_key, decoded, sizeof decoded);
    assert(wl_key_starts == 1 && wl_starts == 1 && !error && wl_serial == 4242 && strcmp(wl_uris, doc_uri) == 0);
    assert(!strstr(wl_uris, scratch) && n == 32 && memcmp(decoded, "/run/user/1000/doc/d1/secret.txt", 32) == 0);
    assert(wl_ended && docs_drag.fd >= 0);
    wl_ended(1, wl_ended_data); /* the drop was read */
    svc_log = fake_docs_log(svc);
    char seen[PATH_MAX + 64];
    snprintf(seen, sizeof seen, "begin\nname secret.txt %s/secret.txt O_PATH\nend 7 done\n", scratch);
    assert(strcmp(svc_log, seen) == 0 && docs_drag.fd < 0);
    puts("PASS a private file dragged out: an O_PATH descriptor of it to the document service, its document's "
         "path in the URI list and the key (never its own), the end told when the drop was read");
    svc = fake_docs(NULL);
    run("Wayland window: a private drag that is cancelled tells the document service",
        (const int[]){ CLICK(3), CLICK(1), CLICK(2), END });
    assert(wl_key_starts == 2 && wl_ended);
    wl_ended(0, wl_ended_data);
    svc_log = fake_docs_log(svc);
    assert(strstr(svc_log, "end 7 cancelled\n") && docs_drag.fd < 0);
    puts("PASS a cancelled private drag ends with end SESSION cancelled");
    wl_on = 0;
    assert(unlink(at_home("Documents/to-secret")) == 0 && unlink(at_home("secret.txt")) == 0 &&
           unlink(at_home("Documents/a b 中.txt")) == 0 && unlink(at_home("Documents/to-etc")) == 0);
    run("reload Documents", (const int[]){ KEY(K_F1 + 4, 0), END });

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

    run("New empty file (Ctrl+Alt+N) + undo, Symlink (L) + undo, Copy path (Ctrl+Shift+C), Invert selection (Ctrl+Shift+I)",
        (const int[]){ KEY('n' & 0x1F, KEYS_CTRL | KEYS_ALT), 'b', 'l', 'a', 'n', 'k', '.', 't', 'x', 't', '\n', END });
    assert(exists("Documents/blank.txt"));
    run("undo new empty file", (const int[]){ CTRL_KEY('z'), END });
    assert(!exists("Documents/blank.txt"));

    clip_paste[0] = '\0';
    run("copy path, symlink + undo, duplicate, invert selection (the menu's)",
        (const int[]){ K_DOWN, KEY('c' & 0x1F, KEYS_CTRL | KEYS_ALT), /* Ctrl+Alt+C: Copy path(s) */
                       'L', '\n', CTRL_KEY('z'),
                       K_DOWN, KEY('u' & 0x1F, KEYS_CTRL),
                       KEY(KEY_INVERT, 0), /* the menu's: Invert selection */ END });
    assert(strcmp(clip_paste, at_home("Documents/c.md")) == 0);
    assert(!exists("Documents/c.md.link") && exists("Documents/c (2).md") && marked(P) == P->nlist);
    for (int i = 0; i < P->nlist; i++) P->list[i].marked = 0;

    int places_before = nplaces;
    mk("Documents/subbm");
    put("Documents/subbm/deep.txt", 0);
    go(at_home("Documents/subbm"), NULL);
    run("Ctrl+D bookmark add/remove, F9 side pane toggle, F4 terminal",
        (const int[]){ KEY('d' & 0x1F, KEYS_CTRL), KEY(K_F1 + 8, 0), KEY(K_F1 + 3, KEYS_SHIFT), END });
    assert(nplaces == places_before + 1 && strcmp(places[nplaces - 1], TRASH_URL) == 0);
    assert(exists(".config/gtk-3.0/bookmarks") && side_hidden == 1 && strcmp(task[0], "/usr/local/bin/term") == 0);
    assert(strstr(msg, "Terminal closed"));
    open_answer = "shown /usr/local/bin/term";
    run("Shift+F4 terminal on one of several screens: open there, not closed", (const int[]){ KEY(K_F1 + 3, KEYS_SHIFT), END });
    assert(strstr(msg, "Opened with term") && !strstr(msg, "closed"));
    open_answer = "back viewer";
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

    /* A cloud drive, as the cloud service's status lists it: read by the
     * real metadata worker, refreshed by the service (here a script that
     * records its calls and adds a file as the remote would). */
    mk("Drives");
    mk("Drives/remote");
    put("Drives/remote/old.txt", 0);
    snprintf(cloud_status_path, sizeof cloud_status_path, "%s", at_home(".cloud-status"));
    snprintf(cloud_client_path, sizeof cloud_client_path, "%s", at_home(".cloud-client"));
    FILE *status = fopen(cloud_status_path, "w");
    assert(status);
    fprintf(status, "remote\t%s\tready\t0\t\nbad/name\t%s\tready\t0\t\nother\t/etc\tready\t0\t\n"
                    "lapsed\t%s\tfailed\t0\tauthentication\n",
            at_home("Drives/remote"), at_home("Drives/bad"), at_home("Drives/lapsed"));
    fclose(status);
    chmod(cloud_status_path, 0644);
    FILE *client = fopen(cloud_client_path, "w");
    assert(client);
    fprintf(client, "#!/bin/sh\nd=$(dirname \"$0\")\nprintf '%%s %%s\\n' \"$1\" \"$2\" >>\"$d/.cloud-calls\"\n"
                    "[ ! -e \"$d/.cloud-fail\" ] || exit 1\n"
                    "[ \"$1\" != --refresh ] || : >\"$2/new.txt\"\n");
    fclose(client);
    chmod(cloud_client_path, 0755);
    FILE *created = fopen(at_home(".cloud-calls"), "w");
    assert(created);
    fclose(created);
    own(".cloud-calls");
    int calls = 0;
#define CLOUD_CALLS() ({ FILE *c = fopen(at_home(".cloud-calls"), "r"); int n = 0, ch; \
                         while (c && (ch = fgetc(c)) != EOF) n += ch == '\n'; if (c) fclose(c); n; })
    go(at_home("Documents"), NULL);
    run("cloud drives are places from the root-owned status; unsafe entries are not",
        (const int[]){ K_BOL, END });
    assert(nclouds == 2 && strcmp(clouds[0].path, at_home("Drives/remote")) == 0);
    assert(strcmp(cloud_state(&clouds[1]), "sign in again") == 0); /* the service's category, not a guess */
    int cloud_place = -1;
    for (int i = 0; i < nplaces; i++) if (strcmp(places[i], at_home("Drives/remote")) == 0) cloud_place = i;
    assert(cloud_place > 0);

    go(at_home("Drives/remote"), NULL);
    run("a cloud folder is read by the worker, not the interface",
        (const int[]){ WAIT, WAIT, WAIT, WAIT, END });
    assert(here("Drives/remote") && P->nlist == 1 && strcmp(P->list[0].name, "old.txt") == 0);
    assert(strcmp(P->list[0].type, "text/plain") == 0); /* by its name: nothing downloaded */
    assert(strstr(drawn_row(1, 0), "[online]"));

    run("F5 on a cloud drive asks the service to refresh, then reads the folder again",
        (const int[]){ KEY(K_F1 + 4, 0), WAIT, WAIT, WAIT, WAIT, WAIT, WAIT, END });
    assert(CLOUD_CALLS() == ++calls && P->nlist == 2 && strcmp(P->list[0].name, "new.txt") == 0);
    unlink(at_home("Drives/remote/new.txt"));

    /* A touch pulled down three rows at the top of the list, then lifted:
     * one refresh. Released short of it, or by a TrackPoint: none. */
    struct pointer_event pull_full[] = {
        { .gesture = GESTURE_SCROLL, .scroll_source = SCROLL_TOUCH, .scroll_phase = SCROLL_BEGIN, .scroll_fingers = 1 },
        { .wheel = 1, .scroll_source = SCROLL_TOUCH, .scroll_phase = SCROLL_UPDATE, .scroll_fingers = 1 },
        { .wheel = 2, .scroll_source = SCROLL_TOUCH, .scroll_phase = SCROLL_UPDATE, .scroll_fingers = 1 },
        { .gesture = GESTURE_SCROLL, .scroll_source = SCROLL_TOUCH, .scroll_phase = SCROLL_END, .scroll_fingers = 1 },
        { .gesture = GESTURE_SCROLL, .scroll_source = SCROLL_TOUCH, .scroll_phase = SCROLL_CANCEL, .scroll_fingers = 1 },
        { .gesture = GESTURE_SCROLL, .scroll_source = SCROLL_CONTINUOUS, .scroll_phase = SCROLL_BEGIN },
        { .wheel = 3, .scroll_source = SCROLL_CONTINUOUS, .scroll_phase = SCROLL_UPDATE },
        { .gesture = GESTURE_SCROLL, .scroll_source = SCROLL_CONTINUOUS, .scroll_phase = SCROLL_END },
    };
    scrolls = pull_full;
    run("pulling down past the threshold at the top refreshes once, on release",
        (const int[]){ SCROLL(0), SCROLL(1), SCROLL(2), SCROLL(3), WAIT, WAIT, WAIT, WAIT, WAIT, WAIT, END });
    assert(scroll_opted == 0 && CLOUD_CALLS() == ++calls && strstr(msg, "updated"));
    unlink(at_home("Drives/remote/new.txt"));
    run("a pull released short of the threshold does not refresh",
        (const int[]){ SCROLL(0), SCROLL(1), SCROLL(3), WAIT, WAIT, END });
    assert(CLOUD_CALLS() == calls);
    run("a cancelled pull (a second finger, a lost screen) does not refresh",
        (const int[]){ WAIT, WAIT, WAIT, SCROLL(0), SCROLL(1), SCROLL(2), SCROLL(4), END });
    assert(CLOUD_CALLS() == calls && strstr(msg, "cancelled"));
    run("a TrackPoint's middle-button scroll never pulls",
        (const int[]){ SCROLL(5), SCROLL(6), SCROLL(7), WAIT, WAIT, END });
    assert(CLOUD_CALLS() == calls);

    put(".cloud-fail", 0);
    int listed = P->nlist;
    run("a failed refresh keeps the previous list and says so",
        (const int[]){ KEY(K_F1 + 4, 0), WAIT, WAIT, WAIT, WAIT, END });
    assert(CLOUD_CALLS() == ++calls && P->nlist == listed && strstr(msg, "previous list"));
    unlink(at_home(".cloud-fail"));

    /* v into a cloud drive (its FUSE without rename2: refused here as rclone
     * refuses it): copied under its own name; the folder read after it keeps
     * quiet, so what was done stays on the line. A failed read still says so. */
    put("Documents/cloud-in.txt", 0);
    char *kept_clip = clip[0];
    int kept_nclip = nclip;
    clip[0] = strdup(at_home("Documents/cloud-in.txt")); /* as c leaves it */
    nclip = 1;
    clip_cut = 0;
    go(at_home("Drives/remote"), NULL);
    run("back in the cloud drive", (const int[]){ WAIT, WAIT, WAIT, WAIT, END });
    refuse_noreplace = 1;
    run("v into a cloud drive without rename2: copied; the read after it does not hide that",
        (const int[]){ CTRL_KEY('v'), WAIT, WAIT, WAIT, WAIT, END });
    refuse_noreplace = 0;
    assert(!error && strstr(msg, "Copied 1 item") &&
           same_text(at_home("Drives/remote/cloud-in.txt"), "Documents/cloud-in.txt"));
    assert(temps_in(at_home("Drives/remote")) == 0 && P->nlist == 2 && index_of("cloud-in.txt") >= 0 &&
           !cloud_job.pid);
    if (!is_root) {
        assert(chmod(at_home("Drives/remote"), 0333) == 0); /* written to, not listed */
        run("v again, and the read after it fails: the failure is said",
            (const int[]){ CTRL_KEY('v'), WAIT, WAIT, WAIT, WAIT, END });
        assert(chmod(at_home("Drives/remote"), 0755) == 0);
        assert(error && strstr(msg, "previous list") && exists("Drives/remote/cloud-in (2).txt"));
        assert(unlink(at_home("Drives/remote/cloud-in (2).txt")) == 0);
    }
    assert(unlink(at_home("Drives/remote/cloud-in.txt")) == 0 && unlink(at_home("Documents/cloud-in.txt")) == 0);
    free(clip[0]);
    clip[0] = kept_clip;
    nclip = kept_nclip;
    puts("PASS a paste into a cloud drive is written and told; the folder read after it reports only a failure");

    go(at_home("Drives"), "remote");
    run("e unmounts a cloud drive through the service, not as a USB drive",
        (const int[]){ 'e', WAIT, WAIT, WAIT, END });
    assert(CLOUD_CALLS() == ++calls && strstr(msg, "unmounted"));
    FILE *log = fopen(at_home(".cloud-calls"), "r");
    char line[PATH_MAX + 32] = "", last[PATH_MAX + 32] = "";
    while (log && fgets(line, sizeof line, log)) snprintf(last, sizeof last, "%s", line);
    if (log) fclose(log);
    char want[PATH_MAX + 32];
    snprintf(want, sizeof want, "--unmount %s\n", at_home("Drives/remote"));
    assert(strcmp(last, want) == 0);
#undef CLOUD_CALLS

    /*
     * Esc leaves files_view only with nothing left to cancel (inside home, to
     * its list); Ctrl+Q, Ctrl+W on the last tab and K_ACT_HOME leave it too;
     * Icons (Ctrl+1), Compact (Ctrl+2) and Details (Ctrl+3) view hitboxes,
     * margins, column-major Compact layout, background menus, rubberband
     * selection, and wheel scrolling decoupled from selection.
     */
    mk("Views");
    mk("Views/dir-a");
    for (int i = 0; i < 85; i++) {
        char rel[64];
        snprintf(rel, sizeof rel, "Views/v-%02d.txt", i + 1);
        put(rel, 1700000000 + i);
    }
    assert(chmod(at_home("Views/v-01.txt"), 0755) == 0);
    go(at_home("Views"), NULL);
    P->view = VIEW_DETAILS;
    P->zoom[VIEW_ICONS] = zoom_default[VIEW_ICONS];
    P->zoom[VIEW_COMPACT] = zoom_default[VIEW_COMPACT];
    P->zoom[VIEW_DETAILS] = zoom_default[VIEW_DETAILS];

    /* 1. Esc in files_view: clears filter, marks, sidebar focus, rubberband
     *    and item drag one at a time; the script's end (Ctrl+Q) leaves. */
    struct click esc_band[] = {
        { .id = AT, .col = SIDE + 60, .row = 3, .button = BTN_LEFT, .clicks = 1 },
        { .id = AT, .col = SIDE + 5, .row = 8 },
        { .id = HIT_FILE + 1, .button = BTN_LEFT, .clicks = 1 },
        { .id = HIT_FILE + 0 },
    };
    clicks = esc_band;
    run("Esc clears filter, marks, sidebar focus, rubberband (restoring marks) and item drag, one at a time",
        (const int[]){ '/', 'v', '-', '0', '\n', 27,
                       KEY('a' & 0x1F, KEYS_CTRL), 27,
                       '\t', 27,
                       ' ', CLICK(0), CLICK(1), 27,
                       CLICK(2), CLICK(3), 27,
                       END });
    assert(extra == 1 && here("Views") && !P->filter[0] && marked(P) == 0 && side == 0 && drag.mode == DRAG_NONE);
    assert(exists("Views/v-01.txt") && !exists("Views/dir-a/v-01.txt"));

    /* 2. Details view: clicking empty space to the right of a name clears marks
     *    without marking that row; right-clicking there opens folder menu;
     *    wheel scrolls P->top without changing P->sel, preserving Shift-click. */
    struct click det_clicks[] = {
        { .id = AT, .col = SIDE + 60, .row = 4, .button = BTN_LEFT, .clicks = 1 },
        { .id = AT, .col = SIDE + 60, .row = 4, .button = BTN_LEFT, .release = 1 },
        { .id = AT, .col = SIDE + 60, .row = 4, .button = BTN_RIGHT, .clicks = 1 },
        { .id = AT, .col = SIDE + 60, .row = 4, .button = BTN_RIGHT, .release = 1 },
        { .id = MENU(0), .button = BTN_LEFT, .clicks = 1 },
        { .id = MENU(0), .button = BTN_LEFT, .release = 1 },
        { .id = AT, .col = SIDE + 20, .row = 6, .wheel = -4 },
        { .id = HIT_FILE + 15, .button = BTN_LEFT, .clicks = 1, .mods = KEYS_SHIFT },
        { .id = HIT_FILE + 15, .button = BTN_LEFT, .release = 1, .mods = KEYS_SHIFT },
    };
    clicks = det_clicks;
    run("Details: blank row tail clears marks and opens folder menu; wheel scrolls without moving selection",
        (const int[]){ KEY('a' & 0x1F, KEYS_CTRL), CLICK(0), CLICK(1),
                       CLICK(2), CLICK(3), CLICK(4), CLICK(5), 'd', 'e', 't', '\n',
                       K_BOL, K_DOWN, K_DOWN, CLICK(6), END });
    assert(exists("Views/det") && marked(P) == 0 && P->sel == 2 && P->top == 12);
    assert(rmdir(at_home("Views/det")) == 0);
    run("Details: Shift-click after wheel scroll marks from the preserved anchor",
        (const int[]){ K_BOL, K_DOWN, K_DOWN, CLICK(6), CLICK(7), CLICK(8), K_DOWN, END });
    assert(marked(P) == 14 && P->list[2].marked && P->list[15].marked && !P->list[1].marked && P->sel == 16);

    /* 3. Icons view (Ctrl+1): card margins leave HIT_AREA between icons on all
     *    sides; marked cards stay visually separated; gap click deselects, gap
     *    rubberband selects a 2x2 block, gap right-click opens folder menu,
     *    right-clicking an unmarked icon clears stale marks, Up on row 0 stays
     *    in column, Backspace goes up, wheel scrolls 1 icon row keeping P->sel. */
    run("Icons: switch to Ctrl+1 and mark first two items",
        (const int[]){ KEY(K_CTRL_0 + 1, KEYS_CTRL), K_BOL, ' ', ' ', K_LEFT, END });
    assert(P->view == VIEW_ICONS && P->list[0].marked && P->list[1].marked && P->sel == 1);
    /* Tile width at default zoom 2 is 14 (gc = 5 across 82 cols), height 5:
     * col SIDE is left gap, SIDE+13 and SIDE+14 are the 2-cell gap between
     * card 0 and card 1, and row 2+4=6 is the bottom gap row. */
    assert(ui_hit_at(SIDE, 2) == HIT_AREA && ui_hit_at(SIDE + 13, 2) == HIT_AREA && ui_hit_at(SIDE + 14, 2) == HIT_AREA);
    assert(ui_hit_at(SIDE + 2, 6) == HIT_AREA);
    assert(drawn_bg[2][SIDE + 2] == MARK && drawn_bg[2][SIDE + 13] == BG && drawn_bg[2][SIDE + 14] == BG);
    assert(drawn_bg[2][SIDE + 16] == ACCENT && drawn_bg[6][SIDE + 2] == BG);

    struct click icon_clicks[] = {
        /* Click in horizontal gap between icon 0 and icon 1 -> clears marks. */
        { .id = AT, .col = SIDE + 13, .row = 3, .button = BTN_LEFT, .clicks = 1 },
        { .id = AT, .col = SIDE + 13, .row = 3, .button = BTN_LEFT, .release = 1 },
        /* Rubberband from gap (SIDE+13, row 2) to card (col 2, row 1) = (SIDE+30, row 8):
         * touches col 1..2 on rows 0..1 -> items 1, 2, 6, 7 (4 items). */
        { .id = AT, .col = SIDE + 13, .row = 2, .button = BTN_LEFT, .clicks = 1 },
        { .id = AT, .col = SIDE + 30, .row = 8 },
        { .id = AT, .col = SIDE + 30, .row = 8, .button = BTN_LEFT, .release = 1 },
        /* Right-click in vertical gap row 6 -> folder menu item 0 ("New folder..."). */
        { .id = AT, .col = SIDE + 4, .row = 6, .button = BTN_RIGHT, .clicks = 1 },
        { .id = AT, .col = SIDE + 4, .row = 6, .button = BTN_RIGHT, .release = 1 },
        { .id = MENU(0), .button = BTN_LEFT, .clicks = 1 },
        { .id = MENU(0), .button = BTN_LEFT, .release = 1 },
        /* Wheel -2 scrolls 2 icon rows (10 items) without changing P->sel. */
        { .id = AT, .col = SIDE + 4, .row = 4, .wheel = -2 },
        /* Right-click unmarked icon 4 while icons 0..1 are marked -> clears old marks. */
        { .id = HIT_FILE + 4, .button = BTN_RIGHT, .clicks = 1 },
    };
    clicks = icon_clicks;
    run("Icons: gap click clears marks, then 2x2 rubberband marks only 4 cards",
        (const int[]){ KEY('a' & 0x1F, KEYS_CTRL), CLICK(0), CLICK(1),
                       CLICK(2), CLICK(3), CLICK(4), END });
    assert(marked(P) == 4 && P->list[1].marked && P->list[2].marked && P->list[6].marked && P->list[7].marked);
    assert(!P->list[0].marked && !P->list[5].marked);

    run("Icons: right-clicking an unmarked icon clears previous multi-selection",
        (const int[]){ K_BOL, ' ', ' ', CLICK(10), 27, END });
    assert(marked(P) == 0 && P->sel == 4);

    run("Icons: gap right-click opens folder menu, Up on row 0 stays in column, wheel keeps selection",
        (const int[]){ CLICK(5), CLICK(6), CLICK(7), CLICK(8), 'i', 'c', 'o', '\n',
                       K_BOL, K_RIGHT, K_RIGHT, K_UP, CLICK(9), END });
    assert(exists("Views/ico") && P->sel == 2 && P->top == 10);
    assert(rmdir(at_home("Views/ico")) == 0);
    run("Icons: Backspace goes up to parent folder", (const int[]){ '\b', END });
    assert(here("") && strcmp(P->list[P->sel].name, "Views") == 0);
    go(at_home("Views"), NULL);

    /* 4. Compact view (Ctrl+2): column-major layout (top-to-bottom per column,
     *    then left-to-right), pixel icons, executable files do not collide with
     *    '*' mark, zoom 3 draws second-line size, horizontal scroll & rubberband. */
    px_fills = 0;
    run("Compact: switch to Ctrl+2, check column-major navigation and marks",
        (const int[]){ KEY(K_CTRL_0 + 2, KEYS_CTRL), K_BOL, END });
    /* nrows = 25, tc = 22 (gc = 3 columns of 25 rows):
     * item 0 ("dir-a") at (SIDE, 2), item 1 ("v-01.txt", exec) at (SIDE, 3),
     * item 25 ("v-25.txt") at (SIDE + 22, 2). */
    assert(P->view == VIEW_COMPACT && px_fills > 40);
    assert(ui_hit_at(SIDE, 2) == HIT_FILE + 0 && ui_hit_at(SIDE, 3) == HIT_FILE + 1);
    assert(ui_hit_at(SIDE + 22, 2) == HIT_FILE + 25);
    assert(drawn[3][SIDE + 2] == ' '); /* executable v-01.txt is NOT '*' unless marked */
    assert(ui_hit_at(SIDE + 18, 3) == HIT_AREA && ui_hit_at(SIDE + 21, 2) == HIT_AREA);

    run("Compact: Down moves within column (+1), Right moves across columns (+25), Left moves back (-25)",
        (const int[]){ K_DOWN, K_RIGHT, K_DOWN, K_LEFT, ' ', END });
    /* 0 -> 1 -> 26 -> 27 -> 2 (marked, then Space advances to 3). */
    assert(P->list[2].marked && P->sel == 3 && drawn[4][SIDE + 2] == '*');

    struct click cmp_clicks[] = {
        /* Click blank space to the right of short filename -> clears marks. */
        { .id = AT, .col = SIDE + 18, .row = 4, .button = BTN_LEFT, .clicks = 1 },
        { .id = AT, .col = SIDE + 18, .row = 4, .button = BTN_LEFT, .release = 1 },
        /* Right-click in column gap -> folder background menu ("New folder..."). */
        { .id = AT, .col = SIDE + 21, .row = 4, .button = BTN_RIGHT, .clicks = 1 },
        { .id = AT, .col = SIDE + 21, .row = 4, .button = BTN_RIGHT, .release = 1 },
        { .id = MENU(0), .button = BTN_LEFT, .clicks = 1 },
        { .id = MENU(0), .button = BTN_LEFT, .release = 1 },
        /* Wheel -1 scrolls 1 column (25 items) horizontally without moving P->sel. */
        { .id = AT, .col = SIDE + 10, .row = 4, .wheel = -1 },
        /* Rubberband auto-scroll right to the last column across 61 items. */
        { .id = AT, .col = SIDE + 18, .row = 2, .button = BTN_LEFT, .clicks = 1 },
        { .id = AT, .col = 99, .row = 6 },
        { .id = AT, .col = 99, .row = 6, .button = BTN_LEFT, .release = 1 },
    };
    clicks = cmp_clicks;
    run("Compact: blank tail clears marks, gap right-click creates folder, wheel scrolls 1 column keeping P->sel",
        (const int[]){ KEY('a' & 0x1F, KEYS_CTRL), CLICK(0), CLICK(1),
                       CLICK(2), CLICK(3), CLICK(4), CLICK(5), 'c', 'm', 'p', '\n',
                       K_BOL, K_DOWN, CLICK(6), END });
    assert(exists("Views/cmp") && marked(P) == 0 && P->sel == 1 && P->top == 25);
    assert(rmdir(at_home("Views/cmp")) == 0);

    run("Compact: horizontal rubberband auto-scrolls across columns",
        (const int[]){ K_BOL, CLICK(7), CLICK(8), TIMEOUT, TIMEOUT, TIMEOUT, CLICK(9), END });
    assert(P->top >= 25 && marked(P) >= 8);
    run("Compact: Esc clears marks and zoom 3 draws subtitle row",
        (const int[]){ KEY('a' & 0x1F, KEYS_CTRL), 27, KEY('+', KEYS_CTRL), KEY('+', KEYS_CTRL), END });
    assert(marked(P) == 0 && P->zoom[VIEW_COMPACT] == 2 && strlen(drawn_row(3, SIDE + 3)) > 0);
    P->zoom[VIEW_COMPACT] = zoom_default[VIEW_COMPACT];
    P->view = VIEW_DETAILS;
    go(scratch, NULL);
    assert(remove_tree(at_home("Views")) == 0);

    /* Native screens reflow one browser state, not a scaled first canvas. */
    native_fixture = native_on = 1;
    native_output = 1;
    native_count = 2;
    files_output_name[0] = '\0';
    select_output(1);
    P->view = VIEW_DETAILS;
    P->follow_sel = 1;
    draw();
    assert(native_output == 1 && files_output == 1 && cell_w == 12 && cell_h == 24);
    assert(output_drawn[0][24][1] != 0 && output_drawn[1][27][1] != 0);
    assert(!memcmp(drawn, output_drawn[1], sizeof drawn));
    int old_top = P->top;
    int which_col, which_row;
    hit_cell(HIT_FILE, &which_col, &which_row);
    mouse = (struct pointer_event){ .output = 0, .col = which_col, .row = which_row,
        .x = which_col * 8 + 4, .y = which_row * 16 + 8, .button = BTN_LEFT, .pressed = 1 };
    pointer_key(&mods);
    assert(native_output == 0 && files_output == 0 && cell_w == 8 && cell_h == 16);
    assert(P->top == old_top && P->sel == 0);
    int local_x, local_y;
    mouse = (struct pointer_event){ .output = 1, .col = 20, .row = 5, .x = 808, .y = 80 };
    select_output(1);
    pointer_xy(&mouse, &local_x, &local_y);
    assert(local_x == 300 && local_y == 120);
    assert(!pointer_here(&(struct pointer_event){ .output = 0 }));
    assert(pointer_here(&(struct pointer_event){ .output = 1 }));
    puts("== native mixed-resolution files: per-output reflow, target hit map and scaled pointer pixels");

    drag.mode = DRAG_BAND;
    drag.base = calloc(1, 1);
    mouse = (struct pointer_event){ .output = 0, .col = 30, .row = 10, .x = 244, .y = 168 };
    pointer_key(&mods);
    assert(drag.mode == DRAG_NONE && files_output == 0);
    select_output(1);
    run("files: hotplug K_SHOWN cancels pixel anchors without changing the folder",
        (const int[]){ HOTPLUG, AFTER_HOTPLUG, END });
    assert(files_output == 0 && native_output == 0 && !strcmp(files_output_name, "eDP-1"));
    native_count = 2;
    native_on = 0;
    int previous, was = files_screens_begin(&previous);
    assert(!was && native_on);
    files_screens_end(was, previous);
    assert(!native_on);
    native_fixture = 0;
    was = files_screens_begin(&previous);
    assert(!was && !native_on);
    draw(); /* an unavailable native canvas stays safely mirrored */
    files_screens_end(was, previous);
    puts("== files: rubberband output transition cancels its anchor; hotplug and mirror fallback stay usable");

    /*
     * Copies, and moves across filesystems: written under a hidden name in
     * the destination folder and renamed when whole. Stopped (Esc, the
     * window's close), failed, or ended by SIGTERM (home's Stop), nothing of
     * them is left under either name. Between chunks the window answers.
     */
    char big[PATH_MAX], downloads[PATH_MAX], final_big[PATH_MAX];
    snprintf(big, sizeof big, "%s", at_home("Documents/big.bin"));
    snprintf(downloads, sizeof downloads, "%s", at_home("Downloads"));
    snprintf(final_big, sizeof final_big, "%s", at_home("Downloads/big.bin"));
    put_bytes(big, 1 << 20); /* 16 chunks */
    own("Documents/big.bin");
    go(at_home("Documents"), NULL);
    copy_tick_interval = 0; /* every chunk looks at the keys */
    snprintf(watch_final, sizeof watch_final, "%s", final_big);
    copy_with("a copy: under a hidden name until whole, the keys looked at between chunks", big, downloads, 0,
              (const int[]){ IDLE, END });
    assert(!error && size_of(final_big) == 1 << 20 && temps_in(downloads) == 0);
    assert(saw_temp && !saw_final && copy_polls >= 15);
    assert(unlink(final_big) == 0);
    puts("PASS a copy is written under a hidden name, renamed when whole; the keys are looked at between chunks");

    copy_with("Esc while copying: y stops it", big, downloads, 0, (const int[]){ 27, 'y', END });
    assert(error && strstr(msg, "Copy stopped") && size_of(final_big) < 0 && temps_in(downloads) == 0);
    assert(size_of(big) == 1 << 20);
    copy_with("Esc while copying: n goes on", big, downloads, 0, (const int[]){ 27, 'n', IDLE, END });
    assert(!error && size_of(final_big) == 1 << 20 && temps_in(downloads) == 0);
    assert(unlink(final_big) == 0);
    puts("PASS Esc while copying asks; y stops it with nothing left, n goes on");

    kept_closes = 0;
    close_asked = 0;
    copy_with("the window's close while copying: y stops it now", big, downloads, 0, (const int[]){ K_CLOSE, 'y', END });
    assert(close_asked && kept_closes == 1 && error && strstr(msg, "Copy stopped"));
    assert(size_of(final_big) < 0 && temps_in(downloads) == 0);
    close_asked = 0;
    copy_with("the close: n closes when the copy is done", big, downloads, 0, (const int[]){ K_CLOSE, 'n', IDLE, END });
    assert(close_asked && kept_closes == 2 && !error && size_of(final_big) == 1 << 20);
    assert(unlink(final_big) == 0);
    close_asked = 0;
    copy_with("the close: Esc keeps the window", big, downloads, 0, (const int[]){ K_CLOSE, 27, IDLE, END });
    assert(!close_asked && kept_closes == 3 && size_of(final_big) == 1 << 20);
    assert(unlink(final_big) == 0);
    copy_with("the close asked again while asked: stopped, closing", big, downloads, 0,
              (const int[]){ K_CLOSE, K_CLOSE, END });
    assert(close_asked && kept_closes == 5 && size_of(final_big) < 0 && temps_in(downloads) == 0);
    close_asked = 0;
    copy_tick_interval = 1e9;
    puts("PASS the window's close while copying is kept (never forced) and asked: stop now, close when done, or stay");

    run("the window's close leaves files as q does", (const int[]){ K_CLOSE, END });
    assert(extra == 0 && files_close_asked());
    int listed_before = P->nlist;
    run("a close during a question: nothing done, files closes", (const int[]){ K_BOL, KEY(K_DEL, KEYS_SHIFT), K_CLOSE, END });
    assert(extra == 0 && files_close_asked() && P->nlist == listed_before && !error);
    assert(pick("choosing: the close ends it, nothing chosen", "text/", at_home("Documents"), NULL,
                (const int[]){ K_CLOSE, END }, chosen, sizeof chosen) == 0 && files_close_asked());
    run("no close: q is back to home's list, nothing else closes", (const int[]){ 'q', END });
    assert(extra == 0 && !files_close_asked());
    puts("PASS the window's close quits files, also from a question and from a choice (files_close_asked)");

    if (!is_root) {
        mk("Documents/tree");
        put("Documents/tree/a.txt", 0);
        put("Documents/tree/b.txt", 0);
        assert(chmod(at_home("Documents/tree/b.txt"), 0) == 0);
        copy_with("a copy that fails midway", at_home("Documents/tree"), downloads, 0, (const int[]){ END });
        assert(error && strstr(msg, "could not be") && !exists("Downloads/tree") && temps_in(downloads) == 0);
        assert(chmod(at_home("Documents/tree/b.txt"), 0644) == 0 && remove_tree(at_home("Documents/tree")) == 0);
        puts("PASS a copy that fails midway leaves nothing under its name and no temporary");
    }

    copy_tick_interval = 0;
    for (int in_question = 0; in_question < 2; in_question++) {
        fflush(NULL);
        pid_t child = fork();
        assert(child >= 0);
        if (child == 0) {
            copy_with("SIGTERM while copying", big, downloads, 0,
                      in_question ? (const int[]){ 27, TERM, END } : (const int[]){ TERM, IDLE, END });
            _exit(0); /* not reached: the signal ends it */
        }
        int status;
        assert(waitpid(child, &status, 0) == child);
        assert(WIFSIGNALED(status) && WTERMSIG(status) == SIGTERM);
        assert(size_of(final_big) < 0 && temps_in(downloads) == 0 && size_of(big) == 1 << 20);
    }
    copy_tick_interval = 1e9;
    puts("PASS SIGTERM (home's Stop) during a copy or its question: the temporary goes, then the signal ends it");

    /* A second filesystem (test-files.sh mounts one): moves are copies there. */
#define OTHER_FS "/run/other"
    struct stat other_st, home_st;
    assert(stat(OTHER_FS, &other_st) == 0 && stat(scratch, &home_st) == 0 && other_st.st_dev != home_st.st_dev);
    assert(mkdir(OTHER_FS "/xfs", 0755) == 0);
    put_bytes(OTHER_FS "/xfs/one.bin", 200000);
    put_bytes(OTHER_FS "/xfs/two.bin", 10);
    copy_with("a move across filesystems", OTHER_FS "/xfs", downloads, 1, (const int[]){ END });
    assert(!error && size_of(at_home("Downloads/xfs/one.bin")) == 200000 && size_of(at_home("Downloads/xfs/two.bin")) == 10);
    assert(access(OTHER_FS "/xfs", F_OK) != 0 && temps_in(downloads) == 0);
    assert(remove_tree(at_home("Downloads/xfs")) == 0);
    if (!is_root) {
        assert(mkdir(OTHER_FS "/xbad", 0755) == 0);
        put_bytes(OTHER_FS "/xbad/a.bin", 10);
        put_bytes(OTHER_FS "/xbad/b.bin", 10);
        assert(chmod(OTHER_FS "/xbad/b.bin", 0) == 0);
        copy_with("a move across filesystems that fails", OTHER_FS "/xbad", downloads, 1, (const int[]){ END });
        assert(error && size_of(at_home("Downloads/xbad")) < 0 && temps_in(downloads) == 0);
        assert(size_of(OTHER_FS "/xbad/a.bin") == 10 && size_of(OTHER_FS "/xbad/b.bin") == 10);
        assert(chmod(OTHER_FS "/xbad/b.bin", 0644) == 0 && remove_tree(OTHER_FS "/xbad") == 0);
    }
    put_bytes(OTHER_FS "/xbig.bin", 1 << 20);
    copy_tick_interval = 0;
    copy_with("Esc during a move across filesystems: y", OTHER_FS "/xbig.bin", downloads, 1, (const int[]){ 27, 'y', END });
    assert(error && strstr(msg, "Move stopped") && size_of(OTHER_FS "/xbig.bin") == 1 << 20);
    assert(size_of(at_home("Downloads/xbig.bin")) < 0 && temps_in(downloads) == 0);
    assert(unlink(OTHER_FS "/xbig.bin") == 0);
    copy_tick_interval = 1e9;
    puts("PASS moves across filesystems are whole or not at all; the source goes only once its copy is whole");

    /* A drive without the no-replace rename (refused here as rclone's FUSE
     * refuses it): written under its own name, created exclusively. Stopped
     * or failed, a file is removed before it is closed (rclone uploads a file
     * when it is closed, never one removed while open). A move into it from
     * another filesystem is such a copy. */
    refuse_noreplace = 1;
    copy_tick_interval = 0;
    copy_with("in place: a copy under its own name", big, downloads, 0, (const int[]){ IDLE, END });
    assert(!error && size_of(final_big) == 1 << 20 && temps_in(downloads) == 0 && saw_temp && saw_final);
    assert(unlink(final_big) == 0);
    closed_unlinked = 0;
    copy_with("in place: Esc y after a few chunks", big, downloads, 0,
              (const int[]){ TIMEOUT, TIMEOUT, TIMEOUT, 27, 'y', END });
    assert(error && strstr(msg, "Copy stopped") && size_of(final_big) < 0 && temps_in(downloads) == 0);
    assert(closed_unlinked == 1 && size_of(big) == 1 << 20);
    copy_tick_interval = 1e9;
    if (!is_root) {
        mk("Documents/tree");
        put("Documents/tree/a.txt", 0);
        put("Documents/tree/b.txt", 0);
        assert(chmod(at_home("Documents/tree/b.txt"), 0) == 0);
        copy_with("in place: a folder that fails midway", at_home("Documents/tree"), downloads, 0, (const int[]){ END });
        assert(error && strstr(msg, "could not be") && !exists("Downloads/tree") && temps_in(downloads) == 0);
        assert(chmod(at_home("Documents/tree/b.txt"), 0644) == 0 && remove_tree(at_home("Documents/tree")) == 0);
    }
    put_bytes(OTHER_FS "/xin.bin", 200000);
    copy_with("in place: a move from another filesystem", OTHER_FS "/xin.bin", downloads, 1, (const int[]){ END });
    assert(!error && strstr(msg, "Moved 1 item") && size_of(at_home("Downloads/xin.bin")) == 200000);
    assert(access(OTHER_FS "/xin.bin", F_OK) != 0 && temps_in(downloads) == 0);
    assert(unlink(at_home("Downloads/xin.bin")) == 0);
    refuse_noreplace = 0;
    puts("PASS without the no-replace rename a copy is written under its own name, exclusively; stopped or failed, "
         "nothing is left and the file had no name when closed; a move from another filesystem is such a copy");

    /*
     * Opening in a window (LOCACHY_BACKEND=wayland, no process 1 to ask): the
     * document viewer through locachy-seat --window view PATH, a container
     * through the top bar's open socket, a folder browsed here, any other
     * framebuffer program refused clearly; never a success that did not
     * happen. Directly, what process 1 says is told as it is.
     */
    snprintf(seat_client_path, sizeof seat_client_path, "%s", at_home(".seat-client"));
    char seat_calls[PATH_MAX], seat_fail[PATH_MAX], doc[PATH_MAX], wtxt[PATH_MAX];
    snprintf(seat_calls, sizeof seat_calls, "%s", at_home(".seat-calls"));
    snprintf(seat_fail, sizeof seat_fail, "%s", at_home(".seat-fail"));
    FILE *seat = fopen(seat_client_path, "w");
    assert(seat);
    fprintf(seat, "#!/bin/sh\nd=$(dirname \"$0\")\nprintf '%%s\\n' \"$*\" >>\"$d/.seat-calls\"\n"
                  "if [ -e \"$d/.seat-fail\" ]; then\n"
                  "    echo \"locachy-seat: the shared display's window control is not running\" >&2\n"
                  "    exit 1\nfi\n");
    assert(fclose(seat) == 0 && chmod(seat_client_path, 0755) == 0);
    put("Documents/doc.pdf", 0);
    put("Documents/w.txt", 0);
    mk("Documents/folder");
    snprintf(doc, sizeof doc, "%s", at_home("Documents/doc.pdf"));
    snprintf(wtxt, sizeof wtxt, "%s", at_home("Documents/w.txt"));
    go(at_home("Documents"), NULL);
    setenv("LOCACHY_BACKEND", "wayland", 1);
    int tasks_before = tasks, opens_before = opens;
    error = 0;
    open_file(doc, 0, "program:view");
    snprintf(want, sizeof want, "--window view %s\n", doc);
    assert(!error && strstr(msg, "Opened with view in a window") && !strcmp(text_of(seat_calls), want));
    assert(tasks == tasks_before);
    put(".seat-fail", 0);
    open_file(doc, 0, "program:view");
    assert(error && strstr(msg, "Cannot open a view window") && strstr(msg, "window control is not running"));
    assert(unlink(seat_fail) == 0 && unlink(seat_calls) == 0);
    puts("PASS window: the viewer opens a view window (locachy-seat --window view PATH); a refusal says why");
    open_file(doc, 0, "program:edit");
    assert(error && strstr(msg, "edit opens from home only") && size_of(seat_calls) < 0 && tasks == tasks_before);
    open_file(at_home("Documents/folder"), 0, "program:view");
    assert(!error && here("Documents/folder") && size_of(seat_calls) < 0);
    go(at_home("Documents"), NULL);
    puts("PASS window: another framebuffer program is refused clearly; a folder is browsed here");

    snprintf(rule, sizeof rule, "one");
    pid_t bar = fake_bar("ok one");
    open_file(wtxt, 0, NULL);
    snprintf(want, sizeof want, "with one %s", wtxt);
    assert(!strcmp(fake_bar_log(bar), want) && !error && strstr(msg, "Opened in one") && opens == opens_before);
    bar = fake_bar("error one is not installed");
    open_file(wtxt, 0, NULL);
    fake_bar_log(bar);
    assert(error && strstr(msg, "one is not installed"));
    assert(unlink(BAR_OPEN_SOCKET) == 0);
    open_file(wtxt, 0, NULL);
    assert(error && strstr(msg, "top bar is not running"));
    open_file(at_home("notes.txt"), 0, NULL);
    assert(error && strstr(msg, "containers cannot see it")); /* checked before the bar is asked */
    rule[0] = '\0';
    puts("PASS window: a container opens through the top bar's open socket (with NAME TARGET); "
         "its refusal or absence is reported; the shared folders' rule stays");

    FILE *home_request = fopen(FILES_REQUEST, "w");
    assert(home_request && fprintf(home_request, "%s\n", downloads) > 0 && fclose(home_request) == 0);
    assert(!take_request() && access(FILES_REQUEST, F_OK) == 0 && unlink(FILES_REQUEST) == 0);
    error = 0;
    open_terminal_here();
    assert(error && strstr(msg, "term opens from home only") && tasks == tasks_before);
    ncalled = 0;
    P->sel = index_of("w.txt");
    archive_action(KEY_EXTRACT_HERE);
    assert(error && strstr(msg, "archive opens from home only") && ncalled == 0);
    unsetenv("LOCACHY_BACKEND");
    puts("PASS window: home's requests stay home's; term and archive say they open from home only");

    open_answer = "no process 1 to ask";
    open_file(doc, 0, "program:viewer");
    assert(error && strstr(msg, "viewer could not be started"));
    open_answer = "ended /usr/local/bin/viewer x -1";
    open_file(doc, 0, "program:viewer");
    assert(error && strstr(msg, "could not be started"));
    open_answer = "back viewer";
    open_file(doc, 0, "program:viewer");
    assert(!error && strstr(msg, "Opened with viewer"));
    puts("PASS direct: an open process 1 did not run is reported so, never as opened");
    assert(unlink(doc) == 0 && unlink(wtxt) == 0 && rmdir(at_home("Documents/folder")) == 0 && unlink(big) == 0);
    assert(unlink(seat_client_path) == 0);

    /* The no-replace rename under a race and on a drive that cannot protect a
     * name: another writer's file is never replaced, and the refusal says
     * plainly that nothing was changed (renameat2 interposed, see above). */
    char race_dir[PATH_MAX], src[PATH_MAX];
    snprintf(race_dir, sizeof race_dir, "%s", at_home("Documents/race"));
    snprintf(src, sizeof src, "%s", at_home("Documents/race/src.txt"));
    assert(mkdir(race_dir, 0755) == 0 && mkdir(at_home("Documents/race/a"), 0755) == 0 &&
           mkdir(at_home("Documents/race/b"), 0755) == 0 && mkdir(at_home("Documents/race/c"), 0755) == 0);
    write_file(src, "source\n");
    snprintf(race_text, sizeof race_text, "other writer\n");
    snprintf(race_target, sizeof race_target, "%s", at_home("Documents/race/taken.txt"));
    assert(go(race_dir, "src.txt") >= 0);
    run("F2 onto a name another writer creates in the gap: refused, neither replaced",
        (const int[]){ KEY(K_F1 + 1, 0), '\b', '\b', '\b', '\b', '\b', '\b', '\b', 't', 'a', 'k', 'e', 'n', '.', 't', 'x', 't', '\n',
                       END });
    assert(error && strstr(msg, "Cannot rename to taken.txt") && !race_target[0]);
    assert(same_text(src, "source\n") && same_text(at_home("Documents/race/taken.txt"), "other writer\n"));
    puts("PASS F2 onto a name another writer creates in the gap: refused, neither file replaced");
    refuse_noreplace = 1;
    run("F2 on a drive that cannot rename without overwrite protection: refused plainly, nothing changed",
        (const int[]){ KEY(K_F1 + 1, 0), '\b', '\b', '\b', '\b', '\b', '\b', '\b', 'b', 'y', 'e', '.', 't', 'x', 't', '\n', END });
    refuse_noreplace = 0;
    assert(error && strstr(msg, "cannot rename without overwrite protection: nothing was changed"));
    assert(same_text(src, "source\n") && !exists("Documents/race/bye.txt"));
    puts("PASS F2 on a drive that cannot rename without overwrite protection: refused plainly, nothing changed");

    snprintf(race_target, sizeof race_target, "%s", at_home("Documents/race/copy.txt"));
    errno = 0;
    assert(copy_item(src, at_home("Documents/race/copy.txt")) < 0 && errno == EEXIST && !race_target[0]);
    assert(same_text(at_home("Documents/race/copy.txt"), "other writer\n") && same_text(src, "source\n"));
    assert(entries_with(race_dir, COPY_TEMP) == 0);
    snprintf(race_target, sizeof race_target, "%s", at_home("Documents/race/moved.txt"));
    errno = 0;
    assert(move_path(src, at_home("Documents/race/moved.txt")) < 0 && errno == EEXIST && !race_target[0]);
    assert(same_text(at_home("Documents/race/moved.txt"), "other writer\n") && same_text(src, "source\n"));
    puts("PASS copy and move onto a name another writer creates in the gap: refused, neither replaced, "
         "no partial copy left");

    /* There a copy is created exclusively under its own name: a file or a
     * folder that is there (created meanwhile) is neither replaced nor
     * removed. A move within that drive is refused plainly. */
    refuse_noreplace = 1;
    assert(no_rename2(race_dir) && entries_with(race_dir, COPY_TEMP) == 0);
    rename_unprotected = 0;
    assert(copy_item(src, at_home("Documents/race/copy2.txt")) == 0 && !rename_unprotected);
    assert(same_text(at_home("Documents/race/copy2.txt"), "source\n") && entries_with(race_dir, COPY_TEMP) == 0);
    errno = 0;
    assert(copy_item(src, at_home("Documents/race/copy.txt")) < 0 && errno == EEXIST);
    assert(same_text(at_home("Documents/race/copy.txt"), "other writer\n") && entries_with(race_dir, COPY_TEMP) == 0);
    write_file(at_home("Documents/race/a/one.txt"), "one\n");
    assert(mkdir(at_home("Documents/race/d"), 0755) == 0);
    write_file(at_home("Documents/race/d/theirs.txt"), "theirs\n");
    errno = 0;
    assert(copy_item(at_home("Documents/race/a"), at_home("Documents/race/d")) < 0 && errno == EEXIST);
    assert(same_text(at_home("Documents/race/d/theirs.txt"), "theirs\n") && !exists("Documents/race/d/one.txt"));
    assert(copy_item(at_home("Documents/race/a"), at_home("Documents/race/a2")) == 0);
    assert(same_text(at_home("Documents/race/a2/one.txt"), "one\n") && entries_with(race_dir, COPY_TEMP) == 0);
    char *moving[] = { src };
    transfer(moving, 1, at_home("Documents/race/c"), 1);
    refuse_noreplace = 0;
    assert(error && strstr(msg, "cannot move within itself without overwrite protection: nothing was moved"));
    assert(same_text(src, "source\n") && !exists("Documents/race/c/src.txt"));
    assert(remove_tree(at_home("Documents/race/a2")) == 0 && remove_tree(at_home("Documents/race/d")) == 0 &&
           unlink(at_home("Documents/race/copy2.txt")) == 0 && unlink(at_home("Documents/race/a/one.txt")) == 0);
    puts("PASS on a drive that cannot rename without overwrite protection: a copy is made exclusively under its "
         "own name, never over a file or folder there; a move within it is refused plainly, both paths as they were");

    /* Bookmarks: while another writer holds the folder's lock and adds its
     * line, two toggles wait for it; afterwards no line of the three is lost. */
    char bdir[PATH_MAX], bpath[PATH_MAX + 16], marks[4096];
    snprintf(bdir, sizeof bdir, "%s/.config/gtk-3.0", home);
    snprintf(bpath, sizeof bpath, "%s/bookmarks", bdir);
    assert(mkdir_p(bdir, 0700) == 0);
    int held = open(bdir, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    assert(held >= 0 && flock(held, LOCK_EX) == 0);
    pid_t kids[2];
    for (int i = 0; i < 2; i++) {
        kids[i] = fork();
        assert(kids[i] >= 0);
        if (kids[i] == 0) {
            go(at_home(i ? "Documents/race/c" : "Documents/race/a"), NULL);
            toggle_bookmark();
            _exit(0);
        }
    }
    FILE *bf = fopen(bpath, "ae");
    assert(bf && fprintf(bf, "file://%s\n", at_home("Documents/race/b")) > 0 && fclose(bf) == 0);
    const char *marks_text = read_file(bpath, marks, sizeof marks);
    assert(marks_text && !strstr(marks_text, "/race/a\n") && !strstr(marks_text, "/race/c\n")); /* both still wait */
    assert(flock(held, LOCK_UN) == 0 && close(held) == 0);
    for (int i = 0; i < 2; i++) {
        int status;
        assert(waitpid(kids[i], &status, 0) == kids[i] && WIFEXITED(status) && WEXITSTATUS(status) == 0);
    }
    marks_text = read_file(bpath, marks, sizeof marks);
    assert(marks_text && strstr(marks_text, "/race/a\n") && strstr(marks_text, "/race/b\n") &&
           strstr(marks_text, "/race/c\n"));
    puts("PASS bookmarks: two toggles wait for the writer holding the lock; no line of the three is lost");

    puts("ALL PASSED");
    return 0;
}
