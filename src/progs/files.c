/*
 * The file manager of home: the user's home (uid 1000), with its shared
 * folders and the Trash as places, laid out as KDE's Dolphin is. Files open
 * in the container of the user's rule for their type, the only one that can
 * open them, or the one picked (o, or a rule of "ask"): the same rules as
 * xdg-open in containers (open.c). Every file operation runs with the user's
 * file permissions (fsuid 1000), so what it creates is the user's and it
 * cannot touch what the user could not.
 *
 * Dolphin's views and keys:
 *   Ctrl+1 Icons, Ctrl+2 Compact, Ctrl+3 Details (the default; its column
 *   headers sort, folders first); Ctrl++ Ctrl+= Ctrl+- Ctrl+0 and Ctrl+wheel
 *   zoom; F3 split view; Ctrl+T, Ctrl+W, Ctrl+Tab, Ctrl+Shift+Tab: tabs
 *   Alt+Left, Alt+Right: back, forward; Alt+Up, Backspace (and Left in the
 *   Details view): up, as far as /; Ctrl+L: the location (trash:// too)
 *   Enter: open (a folder: go in)   Tab: places, the panes   o: open with...
 *   Ctrl+C c, Ctrl+X x, Ctrl+V v: copy, cut, paste   Ctrl+A: select all
 *   Space: mark   F2 r: rename   F10 Ctrl+Shift+N n: new folder
 *   Delete d: move to the Trash   Shift+Delete D: delete permanently
 *   Ctrl+Z u: undo (Trash, rename, new folder, copy, move)
 *   Alt+Return i: properties   Ctrl+H .: hidden files   / Ctrl+I: filter
 *   F5: reload   q/Esc, the home key: back   e: eject (a drive in ~/Drives, which the
 *   drives service mounts there)   w: the image as the wallpaper of the
 *   shared display (rc.conf wallpaper)
 * In trash:// (the FreeDesktop.org Trash 1.0 directories: ~/.local/share/
 * Trash, and a drive's .Trash-1000): r restores, Delete deletes permanently,
 * e empties the Trash.
 * The pointer: a click selects (Ctrl: marks, Shift: marks a range), a double
 * click opens, dragging from empty space marks what the rectangle touches
 * (scrolling at the edges), dragging items drops them into a folder, a
 * place or the other pane (Ctrl: copies), the right button gives a menu, the
 * wheel scrolls; the toolbar, the path, the places, the tabs and the buttons
 * click.
 *
 * A container's request (FILES_REQUEST, from locachy-bar) shows a folder,
 * its newest file selected, or a file's folder with it selected.
 *
 * files_pick is the same browser for choosing one file of a type (settings:
 * the wallpaper): Enter on such a file chooses it; nothing is changed there.
 * Of the type inode/directory (packages: a container's directories), o chooses
 * the selected folder, Enter still goes in.
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <fnmatch.h>
#include <ftw.h>
#include <grp.h>
#include <limits.h>
#include <linux/input-event-codes.h>
#include <pwd.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/fsuid.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <time.h>
#include <unistd.h>

#include "base.h"
#include "archive-proto.h"
#include "clipboard.h"
#include "files.h"

#define FG     0xD0D0D0
#define BG     0x101820
#define ACCENT 0x80C0FF
#define DIM    0x808890
#define ERROR  0xFF9090
#define MARK   0x2C4A68 /* a marked item's background */
#define PANE_BG 0x141E28 /* the inactive pane's */
#define BAR_BG 0x1C2836 /* the toolbar's, the headers', the tabs' */
#define SIDE   18 /* sidebar width, columns */
#define MAX_FILES 100000
#define MAX_PLACES 24
#define MAX_CLIP 256
#define MAX_TABS 12
#define MAX_HISTORY 64
#define MAX_UNDO 16
#define USER 1000
#define TRASH_URL "trash://"
#define SEARCH_URL "search://"
/* What the drawing registers for the pointer. */
#define HIT_FILE  0       /* + the file's index (+ HIT_PANE in the second pane) */
#define HIT_PANE  100000
#define HIT_AREA  200000  /* + the pane: its empty space */
#define HIT_PLACE 300000  /* + the place's index */
#define HIT_PATH  400000  /* + the length of the folder's path (0: /) */
#define HIT_SORT  500000  /* + the pane * 16 + the column */
#define HIT_TAB   600000  /* + the tab */
#define HIT_VIEW  700000  /* + the view mode */
#define HIT_KEY   0x1000000 /* + the key a button stands for, with MODS(its modifiers) */
#define MODS(m)   ((m) << 16)

enum { VIEW_ICONS, VIEW_COMPACT, VIEW_DETAILS, VIEWS };
enum {
    SORT_NAME, SORT_SIZE, SORT_MTIME, SORT_TYPE, SORT_ORIGIN, SORT_DELETED,
    SORT_EXT, SORT_PERM, SORT_OWNER
};
enum {
    COL_SIZE  = 1 << 0,
    COL_MTIME = 1 << 1,
    COL_TYPE  = 1 << 2,
    COL_EXT   = 1 << 3,
    COL_PERM  = 1 << 4,
    COL_OWNER = 1 << 5,
};
enum { KIND_FOLDER, KIND_FILE, KIND_TEXT, KIND_IMAGE, KIND_AUDIO, KIND_VIDEO, KIND_PDF, KIND_ARCHIVE, KIND_EXEC };
static const int zoom_max[VIEWS] = { 6, 2, 2 }, zoom_default[VIEWS] = { 2, 0, 0 };

struct file {
    char name[256];   /* in trash://: its name in the Trash's files */
    int dir, marked, exec, link, items; /* items: child count for folders (-1 until counted) */
    mode_t mode;
    uid_t uid;
    gid_t gid;
    long long size, blocks;
    time_t mtime;
    char type[OPEN_TYPE]; /* "" until it is needed */
    char *troot;      /* trash://: the Trash directory it is in */
    char *origin;     /* trash:// or search://: where it is / was */
    time_t deleted;   /* trash://: .trashinfo's DeletionDate */
};

struct place_visit {
    char *path, *keep; /* a folder and the name selected in it */
};

/* A view of a folder: a tab has one, or two side by side (split). */
struct pane {
    char cwd[PATH_MAX];    /* a folder, TRASH_URL, or SEARCH_URL */
    char search_root[PATH_MAX];
    char search_query[128];
    struct file *list;
    int nlist, nhidden, cap, sel, top, view, sort, desc;
    int zoom[VIEWS];
    char filter[128];
    struct place_visit back[MAX_HISTORY], fwd[MAX_HISTORY];
    int nback, nfwd;
};

struct tab {
    struct pane pane[2];
    int split, active;
};

static struct tab tabs[MAX_TABS];
static int ntabs, cur_tab;
static struct pane *P; /* the active pane of the shown tab */
static int side, side_sel, side_hidden, hidden, error, msg_sticky;
static int cols_mask = COL_SIZE | COL_MTIME | COL_TYPE;
static int mingle_dirs, sort_case;
static char home[256], dirs[256], msg[512];
static char places[MAX_PLACES][PATH_MAX];
static int nplaces;
static char *clip[MAX_CLIP];
static int nclip, clip_cut;
static const char *pick_type; /* files_pick: the type a file must have to be chosen */

/* The pointer's press, held: on an item (it may become a drag of the marked
 * items), or a rubberband from empty space. */
enum { DRAG_NONE, DRAG_PENDING, DRAG_ITEMS, DRAG_BAND };
static struct {
    int mode, pane, item, mods, clear;
    int ax, ay;          /* the band's anchor, in the pane's document pixels */
    int x, y, col, row;  /* the pointer now */
    char *base;          /* the marks at the press */
    int nbase;
} drag;

/* The undo stack (Dolphin's Ctrl+Z: KIO's FileUndoManager). */
enum { UNDO_TRASH, UNDO_RENAME, UNDO_MKDIR, UNDO_COPY, UNDO_MOVE, UNDO_CREATE };
struct undo {
    int kind, n;
    char **from, **to, **info;
};
static struct undo undos[MAX_UNDO];
static int nundo;

static int picking_folder(void)
{
    return pick_type && strcmp(pick_type, "inode/directory") == 0;
}

static void as_user(int on)
{
    setfsgid(on ? USER : 0);
    setfsuid(on ? USER : 0);
}

static int clampi(int x, int lo, int hi)
{
    return x < lo ? lo : x > hi ? hi : x;
}

static int side_width(void)
{
    return side_hidden ? 0 : SIDE;
}

/* The line above the buttons: what the last key did. */
__attribute__((format(printf, 2, 3)))
static void report(int is_error, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);
    error = is_error;
    msg_sticky = 1;
}

static int inside(const char *path, const char *dir)
{
    size_t n = strlen(dir);
    if (strcmp(dir, "/") == 0) {
        return path[0] == '/';
    }
    return strncmp(path, dir, n) == 0 && (path[n] == '/' || path[n] == '\0');
}

static int is_trash(const struct pane *p)
{
    return strcmp(p->cwd, TRASH_URL) == 0;
}

static int is_search(const struct pane *p)
{
    return strcmp(p->cwd, SEARCH_URL) == 0;
}

/* dir/name; -1 if it does not fit, and then nothing is done with it: a
 * shortened path names some other file. */
static int join(char *out, size_t size, const char *dir, const char *name)
{
    return snprintf(out, size, "%s/%s", strcmp(dir, "/") == 0 ? "" : dir, name) < (int)size ? 0 : -1;
}

static const char *base_name(const char *path)
{
    const char *slash = strrchr(path, '/');
    return slash && slash[1] ? slash + 1 : path;
}

/* The folder of path (/ for /x). */
static void parent_of(const char *path, char *out, size_t size)
{
    const char *slash = strrchr(path, '/');
    snprintf(out, size, "%.*s", slash && slash != path ? (int)(slash - path) : 1, slash ? path : "/");
}

static size_t utf_prev(const char *s, size_t p)
{
    if (p) do { p--; } while (p && ((unsigned char)s[p] & 0xC0) == 0x80);
    return p;
}

static size_t utf_next(const char *s, size_t p)
{
    if (s[p]) do { p++; } while (s[p] && ((unsigned char)s[p] & 0xC0) == 0x80);
    return p;
}

static uint32_t utf_at(const char *s, size_t i, size_t next)
{
    unsigned char b = (unsigned char)s[i];
    uint32_t cp = next - i == 1 ? b : b & (0x3Fu >> (next - i - 1));
    for (size_t k = i + 1; k < next; k++) cp = cp << 6 | ((unsigned char)s[k] & 63);
    return cp;
}

static int utf_cells(const char *s, size_t from, size_t to)
{
    int cells = 0;
    for (size_t i = from; i < to && s[i];) {
        size_t next = utf_next(s, i);
        cells += screen_char_cells(utf_at(s, i, next));
        i = next;
    }
    return cells;
}

/* The bytes of the longest start of s within cells. */
static size_t utf_fit(const char *s, int cells)
{
    size_t i = 0;
    int used = 0;
    while (s[i]) {
        size_t next = utf_next(s, i);
        int w = screen_char_cells(utf_at(s, i, next));
        if (used + w > cells) break;
        used += w;
        i = next;
    }
    return i;
}

/* s in at most cells, ending in an ellipsis if it was cut. */
static void fit(char *out, size_t size, const char *s, int cells)
{
    if (cells <= 0) {
        out[0] = '\0';
    } else if (utf_cells(s, 0, strlen(s)) <= cells) {
        snprintf(out, size, "%s", s);
    } else {
        snprintf(out, size, "%.*s\xe2\x80\xa6", (int)utf_fit(s, cells - 1), s);
    }
}

/* s padded with spaces to cells (or cut to them). */
static void pad(char *out, size_t size, const char *s, int cells)
{
    fit(out, size, s, cells);
    size_t len = strlen(out);
    int w = utf_cells(out, 0, len);
    while (w < cells && len + 1 < size) {
        out[len++] = ' ';
        w++;
    }
    out[len] = '\0';
}

/* Natural file name comparison (a2 before a10), optionally case-sensitive. */
static int natural_cmp(const char *a, const char *b, int case_sens)
{
    while (*a && *b) {
        if (isdigit((unsigned char)*a) && isdigit((unsigned char)*b)) {
            const char *x = a, *y = b;
            while (*x == '0') x++;
            while (*y == '0') y++;
            size_t nx = 0, ny = 0;
            while (isdigit((unsigned char)x[nx])) nx++;
            while (isdigit((unsigned char)y[ny])) ny++;
            if (nx != ny) return nx < ny ? -1 : 1;
            int c = strncmp(x, y, nx);
            if (c) return c;
            a = x + nx;
            b = y + ny;
            continue;
        }
        int ca = case_sens ? (unsigned char)*a : tolower((unsigned char)*a);
        int cb = case_sens ? (unsigned char)*b : tolower((unsigned char)*b);
        if (ca != cb) return ca - cb;
        a++;
        b++;
    }
    return (unsigned char)*a - (unsigned char)*b;
}

static void mode_string(mode_t m, char *out)
{
    static const char rwx[] = "rwx";
    out[0] = S_ISDIR(m) ? 'd' : S_ISLNK(m) ? 'l' : '-';
    for (int i = 0; i < 9; i++) {
        out[1 + i] = (m & (1 << (8 - i))) ? rwx[i % 3] : '-';
    }
    out[10] = '\0';
}

static const char *uid_name(uid_t uid)
{
    static struct { uid_t id; int set; char name[32]; } cache[16];
    static int next;
    for (int i = 0; i < 16; i++) {
        if (cache[i].set && cache[i].id == uid) return cache[i].name;
    }
    struct passwd *pw = getpwuid(uid);
    int slot = next++ % 16;
    cache[slot].id = uid;
    cache[slot].set = 1;
    if (pw && pw->pw_name && pw->pw_name[0]) {
        snprintf(cache[slot].name, sizeof cache[slot].name, "%.31s", pw->pw_name);
    } else {
        snprintf(cache[slot].name, sizeof cache[slot].name, "%u", (unsigned)uid);
    }
    return cache[slot].name;
}

static const char *gid_name(gid_t gid)
{
    static struct { gid_t id; int set; char name[32]; } cache[16];
    static int next;
    for (int i = 0; i < 16; i++) {
        if (cache[i].set && cache[i].id == gid) return cache[i].name;
    }
    struct group *gr = getgrgid(gid);
    int slot = next++ % 16;
    cache[slot].id = gid;
    cache[slot].set = 1;
    if (gr && gr->gr_name && gr->gr_name[0]) {
        snprintf(cache[slot].name, sizeof cache[slot].name, "%.31s", gr->gr_name);
    } else {
        snprintf(cache[slot].name, sizeof cache[slot].name, "%u", (unsigned)gid);
    }
    return cache[slot].name;
}

static void owner_string(const struct file *f, char *out, size_t size)
{
    snprintf(out, size, "%.20s:%.20s", uid_name(f->uid), gid_name(f->gid));
}

static void human(long long n, char *out, size_t size)
{
    const char *unit[] = { "B", "KiB", "MiB", "GiB", "TiB" };
    double v = (double)n;
    int u = 0;
    while (v >= 1024 && u < 4) {
        v /= 1024;
        u++;
    }
    snprintf(out, size, u ? "%.1f %s" : "%.0f %s", v, unit[u]);
}

static int remove_one(const char *path, const struct stat *st, int flag, struct FTW *ftw)
{
    (void)st;
    (void)ftw;
    return (flag == FTW_DP ? rmdir(path) : unlink(path)) == 0 ? 0 : -1;
}

static int remove_tree(const char *path)
{
    return nftw(path, remove_one, 16, FTW_DEPTH | FTW_PHYS);
}

static int copy_tree(const char *from, const char *to)
{
    struct stat st;
    if (lstat(from, &st) < 0) {
        return -1;
    }
    if (S_ISLNK(st.st_mode)) {
        char link[PATH_MAX];
        ssize_t n = readlink(from, link, sizeof link - 1);
        if (n < 0) {
            return -1;
        }
        link[n] = '\0';
        return symlink(link, to);
    }
    /* Read, write and execute only, as cp does without -p: no setuid,
     * setgid or sticky bit comes with a copy; the copy is the user's. */
    if (S_ISDIR(st.st_mode)) {
        if (mkdir(to, (st.st_mode & 0777) | 0700) < 0) {
            return -1;
        }
        DIR *d = opendir(from);
        struct dirent *e;
        int failed = d == NULL;
        while (d && (e = readdir(d)) != NULL) {
            char a[PATH_MAX], b[PATH_MAX];
            if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0) {
                continue;
            }
            failed |= join(a, sizeof a, from, e->d_name) < 0 || join(b, sizeof b, to, e->d_name) < 0 ||
                      copy_tree(a, b) < 0;
        }
        if (d) {
            closedir(d);
        }
        return failed ? -1 : 0;
    }
    if (!S_ISREG(st.st_mode)) {
        return 0; /* devices, sockets: not copied */
    }
    int in = open(from, O_RDONLY | O_CLOEXEC), out = -1;
    if (in >= 0) {
        out = open(to, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, st.st_mode & 0777);
    }
    char buf[65536];
    ssize_t n = 0;
    while (in >= 0 && out >= 0 && (n = read(in, buf, sizeof buf)) > 0) {
        if (write(out, buf, (size_t)n) != n) {
            n = -1;
            break;
        }
    }
    int failed = in < 0 || out < 0 || n < 0;
    if (in >= 0) {
        close(in);
    }
    if (out >= 0 && close(out) < 0) {
        failed = 1;
    }
    return failed ? -1 : 0;
}

/* from moved to to, copied and removed across filesystems; never over
 * something that is there. 0, or -1. */
static int move_path(const char *from, const char *to)
{
    struct stat st;
    if (lstat(to, &st) == 0) {
        errno = EEXIST;
        return -1;
    }
    if (rename(from, to) == 0) {
        return 0;
    }
    if (errno != EXDEV) {
        return -1;
    }
    if (copy_tree(from, to) < 0) {
        remove_tree(to);
        return -1;
    }
    return remove_tree(from);
}

/* path and the folders above it, made as needed (mode). */
static int mkdir_p(const char *path, mode_t mode)
{
    char p[PATH_MAX];
    if (snprintf(p, sizeof p, "%s", path) >= (int)sizeof p) {
        return -1;
    }
    for (char *s = p + 1; *s; s++) {
        if (*s == '/') {
            *s = '\0';
            if (mkdir(p, mode) < 0 && errno != EEXIST) return -1;
            *s = '/';
        }
    }
    return mkdir(p, mode) < 0 && errno != EEXIST ? -1 : 0;
}

/* dir/NAME, or dir/NAME (2).EXT... if it is taken; -1 if none fits. */
static int free_name(const char *dir, const char *name, char *out, size_t size)
{
    struct stat st;
    const char *dot = strrchr(name, '.');
    if (!dot || dot == name) {
        dot = name + strlen(name);
    }
    if (join(out, size, dir, name) < 0) {
        return -1;
    }
    for (int i = 2; lstat(out, &st) == 0; i++) {
        if (i == 1000 || snprintf(out, size, "%s/%.*s (%d)%s", strcmp(dir, "/") == 0 ? "" : dir,
                                  (int)(dot - name), name, i, dot) >= (int)size) {
            return -1;
        }
    }
    return 0;
}

/*
 * The Trash, as the FreeDesktop.org Trash specification 1.0 has it: the home
 * trash ~/.local/share/Trash (XDG_DATA_HOME's), and on a drive in ~/Drives,
 * $topdir/.Trash/1000 (where $topdir/.Trash is a sticky folder, not a link)
 * or else $topdir/.Trash-1000. Each holds files/NAME and info/NAME.trashinfo
 * (Path, URL-escaped, absolute in the home trash and relative to $topdir in
 * a drive's; DeletionDate, local time). All of it the user's (fsuid 1000).
 */
static int home_trash(char *out, size_t size)
{
    return snprintf(out, size, "%s/.local/share/Trash", home) < (int)size ? 0 : -1;
}

/* A trash directory and its files and info, made (0700) if they are not
 * there; on the same filesystem as dev (if dev). 0, or -1. */
static int trash_ready(const char *root, dev_t dev)
{
    char sub[PATH_MAX];
    struct stat st;
    if (mkdir_p(root, 0700) < 0 || lstat(root, &st) < 0 || !S_ISDIR(st.st_mode) || (dev && st.st_dev != dev)) {
        return -1;
    }
    for (int i = 0; i < 2; i++) {
        if (join(sub, sizeof sub, root, i ? "info" : "files") < 0 || (mkdir(sub, 0700) < 0 && errno != EEXIST) ||
            lstat(sub, &st) < 0 || !S_ISDIR(st.st_mode)) {
            return -1;
        }
    }
    return 0;
}

/* The mount point of path's filesystem: its highest folder on that device. */
static void mount_top(const char *path, dev_t dev, char *out, size_t size)
{
    char p[PATH_MAX], up[PATH_MAX];
    struct stat st;
    snprintf(p, sizeof p, "%s", path);
    while (strcmp(p, "/") != 0) {
        parent_of(p, up, sizeof up);
        if (stat(up, &st) < 0 || st.st_dev != dev) break;
        snprintf(p, sizeof p, "%s", up);
    }
    snprintf(out, size, "%s", p);
}

/* A drive's trash for $topdir: its .Trash/1000, else its .Trash-1000. */
static int topdir_trash(const char *top, dev_t dev, char *out, size_t size)
{
    char shared[PATH_MAX];
    struct stat st;
    if (join(shared, sizeof shared, top, ".Trash") == 0 && lstat(shared, &st) == 0 && S_ISDIR(st.st_mode) &&
        (st.st_mode & S_ISVTX) && snprintf(out, size, "%s/%d", shared, USER) < (int)size &&
        trash_ready(out, dev) == 0) {
        return 0;
    }
    return snprintf(out, size, "%s/.Trash-%d", top, USER) < (int)size && trash_ready(out, dev) == 0 ? 0 : -1;
}

/* Path= as the specification escapes it (RFC 2396: all but the unreserved
 * characters and /). */
static void url_escape(const char *s, char *out, size_t size)
{
    size_t n = 0;
    for (; *s && n + 4 < size; s++) {
        unsigned char c = (unsigned char)*s;
        if (isalnum(c) || strchr("-_.!~*'()/", c)) {
            out[n++] = (char)c;
        } else {
            n += (size_t)snprintf(out + n, size - n, "%%%02X", c);
        }
    }
    out[n] = '\0';
}

static void url_unescape(const char *s, char *out, size_t size)
{
    size_t n = 0;
    for (; *s && n + 1 < size; s++) {
        if (s[0] == '%' && isxdigit((unsigned char)s[1]) && isxdigit((unsigned char)s[2])) {
            char hex[3] = { s[1], s[2], 0 };
            out[n++] = (char)strtol(hex, NULL, 16);
            s += 2;
        } else {
            out[n++] = *s;
        }
    }
    out[n] = '\0';
}

/* NAME, then NAME.2.EXT, NAME.3.EXT... (as GIO names them). */
static int trash_name(const char *base, int i, char *out, size_t size)
{
    if (i == 1) {
        return snprintf(out, size, "%s", base) < (int)size ? 0 : -1;
    }
    const char *dot = strrchr(base, '.');
    if (!dot || dot == base) {
        dot = base + strlen(base);
    }
    return snprintf(out, size, "%.*s.%d%s", (int)(dot - base), base, i, dot) < (int)size ? 0 : -1;
}

/* path into the Trash: its .trashinfo first (O_EXCL: the name is taken
 * there), then the file. The trashed file's and the info's paths are put in
 * to and info. 0, or -1 (nothing changed). As the user. */
static int trash_one(const char *path, char *to, char *info, size_t size)
{
    char root[PATH_MAX], top[PATH_MAX] = "", drives[PATH_MAX], escaped[PATH_MAX * 3], name[256], date[32];
    struct stat st, hs;
    if (lstat(path, &st) < 0 || home_trash(root, sizeof root) < 0 || trash_ready(root, 0) < 0 ||
        stat(root, &hs) < 0) {
        return -1;
    }
    if (inside(root, path)) {
        errno = EINVAL; /* the Trash itself, or what holds it */
        return -1;
    }
    if (st.st_dev != hs.st_dev && join(drives, sizeof drives, home, "Drives") == 0) {
        /* A drive's own trash where it can be; else the home trash, copied. */
        mount_top(path, st.st_dev, top, sizeof top);
        if (!inside(top, drives) || strcmp(top, path) == 0 || topdir_trash(top, st.st_dev, root, sizeof root) < 0) {
            top[0] = '\0';
            home_trash(root, sizeof root);
        }
    }
    url_escape(top[0] ? path + strlen(top) + 1 : path, escaped, sizeof escaped);
    time_t now = time(NULL);
    strftime(date, sizeof date, "%Y-%m-%dT%H:%M:%S", localtime(&now));
    for (int i = 1; i < 1000; i++) {
        struct stat taken;
        if (trash_name(base_name(path), i, name, sizeof name) < 0 ||
            snprintf(info, size, "%s/info/%s.trashinfo", root, name) >= (int)size ||
            snprintf(to, size, "%s/files/%s", root, name) >= (int)size) {
            return -1;
        }
        int fd = open(info, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
        if (fd < 0 && errno == EEXIST) {
            continue;
        }
        if (fd < 0) {
            return -1;
        }
        if (lstat(to, &taken) == 0) {
            close(fd);
            unlink(info);
            continue;
        }
        FILE *f = fdopen(fd, "w");
        int ok = f && fprintf(f, "[Trash Info]\nPath=%s\nDeletionDate=%s\n", escaped, date) > 0;
        if (f ? fclose(f) != 0 : close(fd) != 0) {
            ok = 0;
        }
        if (!ok || move_path(path, to) < 0) {
            int saved = errno;
            unlink(info);
            errno = saved;
            return -1;
        }
        return 0;
    }
    return -1;
}

/* The trash directories there are: the home trash, and the drives'. */
static int trash_roots(char roots[][PATH_MAX], int max)
{
    int n = 0;
    char drives[PATH_MAX];
    struct stat st;
    if (home_trash(roots[n], PATH_MAX) == 0 && stat(roots[n], &st) == 0) {
        n++;
    }
    DIR *d = join(drives, sizeof drives, home, "Drives") == 0 ? opendir(drives) : NULL;
    struct dirent *e;
    while (d && (e = readdir(d)) != NULL && n + 2 <= max) {
        if (e->d_name[0] == '.') continue;
        if (snprintf(roots[n], PATH_MAX, "%s/%s/.Trash/%d", drives, e->d_name, USER) < PATH_MAX &&
            stat(roots[n], &st) == 0 && S_ISDIR(st.st_mode)) {
            n++;
        }
        if (snprintf(roots[n], PATH_MAX, "%s/%s/.Trash-%d", drives, e->d_name, USER) < PATH_MAX &&
            stat(roots[n], &st) == 0 && S_ISDIR(st.st_mode)) {
            n++;
        }
    }
    if (d) {
        closedir(d);
    }
    return n;
}

/* A .trashinfo: where the file was (absolute) and when it was trashed. */
static int trash_info(const char *root, const char *info, char *origin, size_t size, time_t *deleted)
{
    char line[PATH_MAX * 3 + 16], path[PATH_MAX] = "";
    int head = 0;
    *deleted = 0;
    FILE *in = fopen(info, "re");
    if (!in) {
        return -1;
    }
    while (fgets(line, sizeof line, in)) {
        line[strcspn(line, "\r\n")] = '\0';
        if (strcmp(line, "[Trash Info]") == 0) {
            head = 1;
        } else if (head && strncmp(line, "Path=", 5) == 0) {
            url_unescape(line + 5, path, sizeof path);
        } else if (head && strncmp(line, "DeletionDate=", 13) == 0) {
            struct tm tm = { 0 };
            if (sscanf(line + 13, "%d-%d-%dT%d:%d:%d", &tm.tm_year, &tm.tm_mon, &tm.tm_mday, &tm.tm_hour,
                       &tm.tm_min, &tm.tm_sec) == 6) {
                tm.tm_year -= 1900;
                tm.tm_mon -= 1;
                tm.tm_isdst = -1;
                *deleted = mktime(&tm);
            }
        } else if (line[0] == '[') {
            head = 0;
        }
    }
    fclose(in);
    if (!path[0]) {
        return -1;
    }
    if (path[0] == '/') {
        snprintf(origin, size, "%s", path);
    } else {
        /* Relative: from the folder the trash directory is in ($topdir; the
         * home trash: XDG_DATA_HOME). */
        char top[PATH_MAX], up[PATH_MAX];
        parent_of(root, top, sizeof top);
        if (strcmp(base_name(top), ".Trash") == 0) { /* $topdir/.Trash/1000 */
            parent_of(top, up, sizeof up);
            snprintf(top, sizeof top, "%s", up);
        }
        if (join(origin, size, top, path) < 0) {
            return -1;
        }
    }
    return 0;
}

/* Where the trashed file of a .trashinfo is: root/files/NAME, root/info/NAME.trashinfo. */
static int trash_paths(const char *root, const char *name, char *file, char *info, size_t size)
{
    return snprintf(file, size, "%s/files/%s", root, name) < (int)size &&
           snprintf(info, size, "%s/info/%s.trashinfo", root, name) < (int)size ? 0 : -1;
}

static const char *shown_name(const struct file *f)
{
    return f->origin ? base_name(f->origin) : f->name;
}

static const char *file_ext(const struct file *f)
{
    if (f->dir) return "";
    const char *base = shown_name(f);
    const char *dot = strrchr(base, '.');
    return (dot && dot != base && dot[1]) ? dot + 1 : "";
}

/* Where item i is: in its folder, in the Trash's files, or at its found path. */
static int item_path(const struct pane *p, int i, char *out, size_t size)
{
    const struct file *f = &p->list[i];
    if (f->troot) {
        return snprintf(out, size, "%s/files/%s", f->troot, f->name) < (int)size ? 0 : -1;
    }
    if (is_search(p) && f->origin) {
        return snprintf(out, size, "%s", f->origin) < (int)size ? 0 : -1;
    }
    return join(out, size, p->cwd, f->name);
}

static int dir_items(const struct pane *p, struct file *f, int idx)
{
    if (!f->dir) return 0;
    if (f->items >= 0) return f->items;
    char path[PATH_MAX];
    int count = 0;
    if (item_path(p, idx, path, sizeof path) == 0) {
        as_user(1);
        DIR *d = opendir(path);
        struct dirent *e;
        while (d && (e = readdir(d)) != NULL && count < 99999) {
            if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0) continue;
            if (!hidden && e->d_name[0] == '.') continue;
            count++;
        }
        if (d) closedir(d);
        as_user(0);
    }
    f->items = count;
    return count;
}

/* The type of item i (open_type, as the user sees it), looked up once. */
static const char *item_type(struct pane *p, int i)
{
    struct file *f = &p->list[i];
    if (!f->type[0]) {
        char path[PATH_MAX];
        int ok = 1;
        if (f->dir) {
            snprintf(f->type, sizeof f->type, "inode/directory");
        } else {
            as_user(1);
            ok = item_path(p, i, path, sizeof path) == 0 && open_type(path, f->type, sizeof f->type) == 0;
            as_user(0);
        }
        if (!ok || !f->type[0]) {
            snprintf(f->type, sizeof f->type, "application/octet-stream");
        }
    }
    return f->type;
}

static void free_entry(struct file *f)
{
    free(f->troot);
    free(f->origin);
    f->troot = f->origin = NULL;
}

static struct file *new_entry(struct pane *p)
{
    if (p->nlist == p->cap) {
        int cap = p->cap ? p->cap * 2 : 256;
        struct file *l = p->cap >= MAX_FILES ? NULL : realloc(p->list, (size_t)cap * sizeof *l);
        if (!l) {
            return NULL;
        }
        p->list = l;
        p->cap = cap;
    }
    struct file *f = &p->list[p->nlist++];
    memset(f, 0, sizeof *f);
    return f;
}

static int shown(const char *name, const char *filter)
{
    return (hidden || name[0] != '.') && (!filter[0] || strcasestr(name, filter));
}

static void stat_entry(struct file *f, const char *path)
{
    struct stat st = { 0 }, lst = { 0 };
    f->link = lstat(path, &lst) == 0 && S_ISLNK(lst.st_mode);
    if (stat(path, &st) < 0) {
        st = lst;
    }
    f->dir = S_ISDIR(st.st_mode);
    f->exec = S_ISREG(st.st_mode) && (st.st_mode & 0111);
    f->items = -1;
    f->mode = f->link ? lst.st_mode : st.st_mode;
    f->uid = st.st_uid;
    f->gid = st.st_gid;
    f->size = st.st_size;
    f->blocks = st.st_blocks;
    f->mtime = st.st_mtime;
}

static void load_folder(struct pane *p)
{
    p->nhidden = 0;
    as_user(1);
    DIR *d = opendir(p->cwd);
    if (!d) {
        as_user(0);
        report(1, _("Cannot read %s."), p->cwd);
        return;
    }
    struct dirent *e;
    struct file *f;
    while ((e = readdir(d)) != NULL) {
        char path[PATH_MAX];
        if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0) {
            continue;
        }
        if (!hidden && e->d_name[0] == '.') {
            p->nhidden++;
            continue;
        }
        if (!shown(e->d_name, p->filter) ||
            join(path, sizeof path, p->cwd, e->d_name) < 0 || strlen(e->d_name) >= sizeof f->name ||
            (f = new_entry(p)) == NULL) {
            continue;
        }
        snprintf(f->name, sizeof f->name, "%s", e->d_name);
        stat_entry(f, path);
    }
    closedir(d);
    as_user(0);
}

static struct pane *search_pane;
static int search_glob;

static int search_visit(const char *path, const struct stat *st, int flag, struct FTW *ftw)
{
    (void)st;
    (void)flag;
    if (ftw->level == 0) return 0;
    const char *base = path + ftw->base;
    if (!hidden && base[0] == '.') {
        return flag == FTW_D ? FTW_SKIP_SUBTREE : 0;
    }
    if (search_pane->nlist >= 5000) return 1;
    int match = search_glob ? fnmatch(search_pane->search_query, base, FNM_CASEFOLD) == 0 :
                              strcasestr(base, search_pane->search_query) != NULL;
    if (!match || !shown(base, search_pane->filter) || strlen(base) >= 256) {
        return 0;
    }
    struct file *f = new_entry(search_pane);
    if (!f) return 1;
    snprintf(f->name, sizeof f->name, "%s", base);
    stat_entry(f, path);
    f->origin = strdup(path);
    if (!f->origin) {
        free_entry(f);
        search_pane->nlist--;
    }
    return 0;
}

static void load_search(struct pane *p)
{
    p->nhidden = 0;
    if (!p->search_root[0] || !p->search_query[0]) return;
    search_pane = p;
    search_glob = strpbrk(p->search_query, "*?[") != NULL;
    as_user(1);
    nftw(p->search_root, search_visit, 16, FTW_PHYS | FTW_ACTIONRETVAL);
    as_user(0);
}

/* trash://: what every trash directory's info lists. */
static void load_trash(struct pane *p)
{
    char roots[16][PATH_MAX];
    as_user(1);
    int n = trash_roots(roots, 16);
    for (int r = 0; r < n; r++) {
        char infos[PATH_MAX];
        DIR *d = join(infos, sizeof infos, roots[r], "info") == 0 ? opendir(infos) : NULL;
        struct dirent *e;
        while (d && (e = readdir(d)) != NULL) {
            char name[256], file[PATH_MAX], info[PATH_MAX], origin[PATH_MAX];
            size_t len = strlen(e->d_name);
            time_t deleted;
            struct stat st;
            struct file *f;
            if (len <= 10 || len - 10 >= sizeof name || strcmp(e->d_name + len - 10, ".trashinfo") != 0) {
                continue;
            }
            snprintf(name, sizeof name, "%.*s", (int)(len - 10), e->d_name);
            if (trash_paths(roots[r], name, file, info, sizeof file) < 0 || lstat(file, &st) < 0 ||
                trash_info(roots[r], info, origin, sizeof origin, &deleted) < 0 ||
                !shown(base_name(origin), p->filter) || (f = new_entry(p)) == NULL) {
                continue;
            }
            snprintf(f->name, sizeof f->name, "%s", name);
            stat_entry(f, file);
            f->troot = strdup(roots[r]);
            f->origin = strdup(origin);
            f->deleted = deleted;
            if (!f->troot || !f->origin) {
                free_entry(f);
                p->nlist--;
            }
        }
        if (d) {
            closedir(d);
        }
    }
    as_user(0);
}

/* The sort of a pane where it is: the Trash's columns are the deletion
 * date and the original location in place of the date and the type. */
static int sort_key(const struct pane *p)
{
    if (is_trash(p)) {
        return p->sort == SORT_MTIME ? SORT_DELETED : p->sort == SORT_TYPE ? SORT_ORIGIN : p->sort;
    }
    if (is_search(p) && p->sort == SORT_TYPE) {
        return SORT_ORIGIN;
    }
    return p->sort == SORT_DELETED ? SORT_MTIME : p->sort == SORT_ORIGIN ? SORT_TYPE : p->sort;
}

static int sort_by, sort_desc;

/* Folders first unless mingle_dirs is on, then the sort's key, then the name. */
static int compare(const void *a, const void *b)
{
    const struct file *x = a, *y = b;
    if (!mingle_dirs && x->dir != y->dir) {
        return y->dir - x->dir;
    }
    int c = 0;
    if (sort_by == SORT_SIZE) {
        c = (x->size > y->size) - (x->size < y->size);
    } else if (sort_by == SORT_MTIME) {
        c = (x->mtime > y->mtime) - (x->mtime < y->mtime);
    } else if (sort_by == SORT_DELETED) {
        c = (x->deleted > y->deleted) - (x->deleted < y->deleted);
    } else if (sort_by == SORT_TYPE) {
        c = strcmp(x->type, y->type);
    } else if (sort_by == SORT_ORIGIN) {
        c = natural_cmp(x->origin ? x->origin : "", y->origin ? y->origin : "", sort_case);
    } else if (sort_by == SORT_EXT) {
        c = natural_cmp(file_ext(x), file_ext(y), sort_case);
    } else if (sort_by == SORT_PERM) {
        char pa[16], pb[16];
        mode_string(x->mode, pa);
        mode_string(y->mode, pb);
        c = strcmp(pa, pb);
    } else if (sort_by == SORT_OWNER) {
        char oa[64], ob[64];
        owner_string(x, oa, sizeof oa);
        owner_string(y, ob, sizeof ob);
        c = strcmp(oa, ob);
    }
    if (!c) c = natural_cmp(shown_name(x), shown_name(y), sort_case);
    if (!c) c = strcmp(x->name, y->name);
    return sort_desc ? -c : c;
}

/* p sorted, keep (a name) selected if it is there. */
static void sort_pane(struct pane *p, const char *keep)
{
    char name[256];
    snprintf(name, sizeof name, "%s", keep ? keep : "");
    sort_by = sort_key(p);
    sort_desc = p->desc;
    for (int i = 0; sort_by == SORT_TYPE && i < p->nlist; i++) {
        item_type(p, i);
    }
    qsort(p->list, (size_t)p->nlist, sizeof *p->list, compare);
    p->sel = 0;
    for (int i = 0; name[0] && i < p->nlist; i++) {
        if (strcmp(p->list[i].name, name) == 0) {
            p->sel = i;
        }
    }
}

/* p's folder (or Trash, or Search) as the user sees it, the filter applied;
 * keep (a name) selected if it is there. */
static void load_pane(struct pane *p, const char *keep)
{
    char name[256];
    snprintf(name, sizeof name, "%s", keep ? keep : "");
    for (int i = 0; i < p->nlist; i++) {
        free_entry(&p->list[i]);
    }
    p->nlist = p->nhidden = p->sel = 0;
    if (is_trash(p)) {
        load_trash(p);
    } else if (is_search(p)) {
        load_search(p);
    } else {
        load_folder(p);
    }
    sort_pane(p, name);
}

static void load_dir(const char *keep)
{
    load_pane(P, keep);
}

static const char *selected_name(const struct pane *p)
{
    return p->nlist ? p->list[p->sel].name : "";
}

/* Every pane of every tab read again, each keeping its selected item. */
static void reload_all(void)
{
    for (int t = 0; t < ntabs; t++) {
        for (int k = 0; k <= tabs[t].split; k++) {
            struct pane *p = &tabs[t].pane[k];
            char keep[256];
            snprintf(keep, sizeof keep, "%s", selected_name(p));
            load_pane(p, keep);
        }
    }
}

static void visit_free(struct place_visit *v)
{
    free(v->path);
    free(v->keep);
    v->path = v->keep = NULL;
}

static void visit_push(struct place_visit *stack, int *n, const char *path, const char *keep)
{
    if (*n == MAX_HISTORY) {
        visit_free(&stack[0]);
        memmove(stack, stack + 1, (MAX_HISTORY - 1) * sizeof *stack);
        (*n)--;
    }
    stack[*n].path = strdup(path);
    stack[*n].keep = strdup(keep);
    if (stack[*n].path && stack[*n].keep) {
        (*n)++;
    } else {
        visit_free(&stack[*n]);
    }
}

/* p shows dir (a folder, or TRASH_URL), keep selected; the place it leaves
 * goes into its history (record). 0, or -1 if the user cannot open it. */
static int go_pane(struct pane *p, const char *dir, const char *keep, int record)
{
    char real[PATH_MAX];
    if (strcmp(dir, TRASH_URL) == 0 || strcmp(dir, SEARCH_URL) == 0) {
        snprintf(real, sizeof real, "%s", dir);
    } else {
        as_user(1);
        DIR *d = realpath(dir, real) ? opendir(real) : NULL;
        int saved = errno;
        if (d) {
            closedir(d);
        }
        as_user(0);
        if (!d) {
            /* TRANSLATORS: first %s is a folder path, second %s is the system error. */
            report(1, _("Cannot open %s: %s."), dir, strerror(saved));
            return -1;
        }
    }
    if (record && p->cwd[0] && (strcmp(real, p->cwd) != 0 || strcmp(real, SEARCH_URL) == 0)) {
        visit_push(p->back, &p->nback, p->cwd, selected_name(p));
        while (p->nfwd > 0) {
            visit_free(&p->fwd[--p->nfwd]);
        }
    }
    snprintf(p->cwd, sizeof p->cwd, "%s", real);
    p->filter[0] = '\0';
    msg_sticky = 0;
    load_pane(p, keep);
    p->top = 0;
    return 0;
}

static int go(const char *dir, const char *keep)
{
    return go_pane(P, dir, keep, 1);
}

/* Back (Alt+Left) or forward (Alt+Right) in the pane's history. */
static void history(int forward)
{
    struct place_visit *from = forward ? P->fwd : P->back, *to = forward ? P->back : P->fwd;
    int *nfrom = forward ? &P->nfwd : &P->nback, *nto = forward ? &P->nback : &P->nfwd;
    if (*nfrom == 0) {
        report(0, forward ? _("Nothing to go forward to.") : _("Nothing to go back to."));
        return;
    }
    struct place_visit v = from[--*nfrom];
    char here[PATH_MAX], keep[256];
    snprintf(here, sizeof here, "%s", P->cwd);
    snprintf(keep, sizeof keep, "%s", selected_name(P));
    if (go_pane(P, v.path, v.keep, 0) == 0) {
        visit_push(to, nto, here, keep);
    }
    visit_free(&v);
}

/* Up (Alt+Up, Backspace): the folder above, as far as /. */
static void go_up(void)
{
    char up[PATH_MAX], keep[256];
    if (is_search(P) && P->search_root[0]) {
        go(P->search_root, NULL);
        return;
    }
    if (is_trash(P)) {
        /* TRANSLATORS: Alt+Left is the key; keep it. */
        report(0, _("The Trash is a place of its own: Alt+Left goes back."));
        return;
    }
    if (strcmp(P->cwd, "/") == 0) {
        return;
    }
    snprintf(keep, sizeof keep, "%.255s", base_name(P->cwd));
    parent_of(P->cwd, up, sizeof up);
    go(up, keep);
}

static void pane_free(struct pane *p)
{
    for (int i = 0; i < p->nlist; i++) {
        free_entry(&p->list[i]);
    }
    free(p->list);
    for (int i = 0; i < p->nback; i++) visit_free(&p->back[i]);
    for (int i = 0; i < p->nfwd; i++) visit_free(&p->fwd[i]);
    memset(p, 0, sizeof *p);
}

/* A new pane like p: its place, view, sort and zoom; its own history. */
static void pane_like(struct pane *to, const struct pane *p)
{
    memset(to, 0, sizeof *to);
    to->view = p->view;
    to->sort = p->sort;
    to->desc = p->desc;
    snprintf(to->search_root, sizeof to->search_root, "%s", p->search_root);
    snprintf(to->search_query, sizeof to->search_query, "%s", p->search_query);
    memcpy(to->zoom, p->zoom, sizeof to->zoom);
    if (go_pane(to, p->cwd, selected_name(p), 0) < 0) {
        snprintf(to->cwd, sizeof to->cwd, "%s", p->cwd);
    }
    to->sel = p->sel < to->nlist ? p->sel : 0;
}

static void set_active(void)
{
    P = &tabs[cur_tab].pane[tabs[cur_tab].active];
}

/* Ctrl+T: a tab showing the active pane's place (as Dolphin's). */
static void new_tab(void)
{
    if (ntabs == MAX_TABS) {
        report(1, P_("There is %d tab already.", "There are %d tabs already.", MAX_TABS), MAX_TABS);
        return;
    }
    struct tab *t = &tabs[ntabs];
    memset(t, 0, sizeof *t);
    pane_like(&t->pane[0], P);
    cur_tab = ntabs++;
    set_active();
    /* TRANSLATORS: first %d is the current tab number, second %d is the total number of tabs. */
    report(0, _("Tab %d of %d."), cur_tab + 1, ntabs);
}

static void close_tab(int which)
{
    if (ntabs == 1) {
        report(0, _("This is the only tab."));
        return;
    }
    pane_free(&tabs[which].pane[0]);
    pane_free(&tabs[which].pane[1]);
    memmove(&tabs[which], &tabs[which + 1], (size_t)(ntabs - which - 1) * sizeof *tabs);
    memset(&tabs[--ntabs], 0, sizeof *tabs);
    if (cur_tab > which || cur_tab >= ntabs) {
        cur_tab = cur_tab > 0 ? cur_tab - 1 : 0;
    }
    set_active();
    /* TRANSLATORS: first %d is the current tab number, second %d is the total number of tabs. */
    report(0, _("Tab closed; tab %d of %d."), cur_tab + 1, ntabs);
}

static void switch_tab(int to)
{
    cur_tab = (to + ntabs) % ntabs;
    set_active();
    char keep[256];
    snprintf(keep, sizeof keep, "%s", selected_name(P));
    load_pane(P, keep);
}

/* F3: a second pane beside the first (at the same place, active), or the
 * active one closed (Dolphin's default). */
static void toggle_split(void)
{
    struct tab *t = &tabs[cur_tab];
    if (!t->split) {
        pane_like(&t->pane[1], &t->pane[0]);
        t->split = 1;
        t->active = 1;
        /* TRANSLATORS: Tab and F3 are keys; keep them. */
        report(0, _("Split: Tab goes from pane to pane, F3 closes the active one."));
    } else {
        struct pane *keep = &t->pane[!t->active];
        pane_free(&t->pane[t->active]);
        if (t->active == 0) {
            t->pane[0] = *keep;
            memset(keep, 0, sizeof *keep);
        }
        t->split = 0;
        t->active = 0;
        report(0, _("One pane."));
    }
    set_active();
}

static int yes(const char *question);
static const char *prompt(const char *question, const char *initial, int select_all);

static int marked(const struct pane *p)
{
    int n = 0;
    for (int i = 0; i < p->nlist; i++) {
        n += p->list[i].marked;
    }
    return n;
}

/* The targets of an operation: the marked items, else the selected one. */
static int targets(const struct pane *p, int *idx, int max)
{
    int n = 0, any = marked(p);
    for (int i = 0; i < p->nlist && n < max; i++) {
        if (p->list[i].marked || (!any && i == p->sel)) {
            idx[n++] = i;
        }
    }
    return n;
}

static void undo_free(struct undo *u)
{
    for (int i = 0; i < u->n; i++) {
        free(u->from[i]);
        free(u->to[i]);
        free(u->info[i]);
    }
    free(u->from);
    free(u->to);
    free(u->info);
    memset(u, 0, sizeof *u);
}

/* A new record on the undo stack (the oldest goes when it is full). */
static struct undo *undo_begin(int kind)
{
    if (nundo == MAX_UNDO) {
        undo_free(&undos[0]);
        memmove(undos, undos + 1, (MAX_UNDO - 1) * sizeof *undos);
        memset(&undos[--nundo], 0, sizeof *undos);
    }
    struct undo *u = &undos[nundo++];
    memset(u, 0, sizeof *u);
    u->kind = kind;
    return u;
}

static void undo_add(struct undo *u, const char *from, const char *to, const char *info)
{
    char **f = realloc(u->from, (size_t)(u->n + 1) * sizeof *f);
    if (f) u->from = f;
    char **t = realloc(u->to, (size_t)(u->n + 1) * sizeof *t);
    if (t) u->to = t;
    char **i = realloc(u->info, (size_t)(u->n + 1) * sizeof *i);
    if (i) u->info = i;
    if (!f || !t || !i) {
        return;
    }
    u->from[u->n] = strdup(from ? from : "");
    u->to[u->n] = strdup(to ? to : "");
    u->info[u->n] = strdup(info ? info : "");
    u->n++;
}

/* The record kept if it has something to undo. */
static void undo_end(struct undo *u)
{
    if (u->n == 0) {
        undo_free(u);
        nundo--;
    }
}

/* Ctrl+Z: the last operation undone (the Trash's items put back, a rename
 * or a move reversed, a new folder removed if it is empty, copies deleted
 * when the user says so), never over something that is there now. */
static void undo_last(void)
{
    static const char *const what[] = {
        N_("moved back from the Trash"), N_("renamed back"),
        N_("removed the new folder"), N_("deleted the copies"),
        N_("moved back"), N_("removed the created file")
    };
    if (nundo == 0) {
        report(0, _("Nothing to undo."));
        return;
    }
    struct undo *u = &undos[nundo - 1];
    char question[128];
    snprintf(question, sizeof question,
             P_("Undo the copy: delete the %d copied item permanently?",
                "Undo the copy: delete the %d copied items permanently?", u->n),
             u->n);
    if (u->kind == UNDO_COPY && !yes(question)) {
        report(0, _("Nothing undone."));
        return;
    }
    int done = 0, failed = 0;
    as_user(1);
    for (int i = u->n - 1; i >= 0; i--) {
        char up[PATH_MAX];
        int ok;
        if (u->kind == UNDO_MKDIR) {
            ok = rmdir(u->to[i]) == 0;
        } else if (u->kind == UNDO_CREATE) {
            struct stat st;
            ok = lstat(u->to[i], &st) == 0 && (S_ISLNK(st.st_mode) || st.st_size == 0) && unlink(u->to[i]) == 0;
        } else if (u->kind == UNDO_COPY) {
            ok = remove_tree(u->to[i]) == 0;
        } else {
            parent_of(u->from[i], up, sizeof up);
            ok = (u->kind != UNDO_TRASH || mkdir_p(up, 0755) == 0) && move_path(u->to[i], u->from[i]) == 0;
            if (ok && u->kind == UNDO_TRASH) {
                unlink(u->info[i]);
            }
        }
        done += ok;
        failed += !ok;
    }
    as_user(0);
    if (failed) {
        /* TRANSLATORS: %s is the undone action, %d is the count of items undone. */
        report(1, P_("Undone: %s (%d item); some could not be (is something there now?).",
                     "Undone: %s (%d items); some could not be (is something there now?).", done),
               _(what[u->kind]), done);
    } else {
        /* TRANSLATORS: %s is the undone action, %d is the count of items undone. */
        report(0, P_("Undone: %s (%d item).", "Undone: %s (%d items).", done), _(what[u->kind]), done);
    }
    undo_free(u);
    nundo--;
    reload_all();
}

/* paths copied (or moved: cut) into dir, each under a free name; recorded
 * for undo. */
static void transfer(char *const *paths, int n, const char *dir, int cut)
{
    int done = 0, failed = 0;
    struct undo *u = undo_begin(cut ? UNDO_MOVE : UNDO_COPY);
    as_user(1);
    for (int i = 0; i < n; i++) {
        const char *base = base_name(paths[i]);
        char to[PATH_MAX], from_dir[PATH_MAX];
        int ok;
        parent_of(paths[i], from_dir, sizeof from_dir);
        if (inside(dir, paths[i])) {
            ok = 0; /* into itself */
        } else if (cut && strcmp(from_dir, dir) == 0) {
            ok = 1; /* moved to where it is */
        } else if (free_name(dir, base, to, sizeof to) < 0) {
            ok = 0;
        } else if (cut) {
            ok = move_path(paths[i], to) == 0;
            if (ok) undo_add(u, paths[i], to, NULL);
        } else {
            ok = copy_tree(paths[i], to) == 0;
            if (ok) undo_add(u, paths[i], to, NULL);
        }
        done += ok;
        failed += !ok;
    }
    as_user(0);
    undo_end(u);
    if (cut && failed) {
        report(1, P_("Moved %d item; some could not be.", "Moved %d items; some could not be.", done), done);
    } else if (cut) {
        /* TRANSLATORS: Ctrl+Z is the key; keep it. */
        report(0, P_("Moved %d item (Ctrl+Z to undo).", "Moved %d items (Ctrl+Z to undo).", done), done);
    } else if (failed) {
        report(1, P_("Copied %d item; some could not be.", "Copied %d items; some could not be.", done), done);
    } else {
        /* TRANSLATORS: Ctrl+Z is the key; keep it. */
        report(0, P_("Copied %d item (Ctrl+Z to undo).", "Copied %d items (Ctrl+Z to undo).", done), done);
    }
}

static void paste(void)
{
    if (is_trash(P)) {
        /* TRANSLATORS: Delete is the key; keep it. */
        report(1, _("Nothing is pasted into the Trash: Delete moves items there."));
        return;
    }
    transfer(clip, nclip, P->cwd, clip_cut);
    if (clip_cut) {
        for (int i = 0; i < nclip; i++) {
            free(clip[i]);
        }
        nclip = 0;
    }
    reload_all();
}

static void to_clip(int cut)
{
    int idx[MAX_CLIP];
    for (int i = 0; i < nclip; i++) {
        free(clip[i]);
    }
    nclip = 0;
    if (is_trash(P)) {
        /* TRANSLATORS: r is the key; keep it. */
        report(1, _("Items in the Trash are restored (r), not copied."));
        return;
    }
    int n = targets(P, idx, MAX_CLIP);
    for (int i = 0; i < n; i++) {
        char path[PATH_MAX];
        if (item_path(P, idx[i], path, sizeof path) == 0 && (clip[nclip] = strdup(path)) != NULL) {
            nclip++;
        }
    }
    clip_cut = cut;
    if (cut) {
        /* TRANSLATORS: v is the key; keep it. */
        report(0, P_("Cut %d item; v puts it in the folder shown.",
                     "Cut %d items; v puts them in the folder shown.", nclip), nclip);
    } else {
        /* TRANSLATORS: v is the key; keep it. */
        report(0, P_("Copied %d item; v puts it in the folder shown.",
                     "Copied %d items; v puts them in the folder shown.", nclip), nclip);
    }
}

/* "NAME" for one item, "the N items" for more. */
static void describe(const struct pane *p, const int *idx, int n, char *out, size_t size)
{
    if (n == 1) {
        snprintf(out, size, "%s", shown_name(&p->list[idx[0]]));
    } else {
        snprintf(out, size, P_("the %d item", "the %d items", n), n);
    }
}

/* Delete (outside trash://): the targets into the Trash, no question asked
 * (Dolphin's), recorded for undo. */
static void trash_items(struct pane *p, const int *idx, int n)
{
    char what[300], why[128] = "";
    int done = 0;
    if (n == 0) {
        return;
    }
    describe(p, idx, n, what, sizeof what);
    struct undo *u = undo_begin(UNDO_TRASH);
    as_user(1);
    for (int i = 0; i < n; i++) {
        char path[PATH_MAX], to[PATH_MAX], info[PATH_MAX];
        if (item_path(p, idx[i], path, sizeof path) == 0 && trash_one(path, to, info, sizeof to) == 0) {
            undo_add(u, path, to, info);
            done++;
        } else {
            snprintf(why, sizeof why, "%s", strerror(errno));
        }
    }
    as_user(0);
    undo_end(u);
    if (done == n) {
        /* TRANSLATORS: %s is a file name or "the N items"; Ctrl+Z is the key; keep it. */
        report(0, _("Moved %s to Trash (Ctrl+Z to undo)."), what);
    } else if (done) {
        /* TRANSLATORS: %d is the count moved, first %s is "the N items", second %s is the system error; Ctrl+Z is the key. */
        report(1, _("Moved %d of %s to Trash (Ctrl+Z to undo); the others could not be: %s."), done, what, why);
    } else {
        /* TRANSLATORS: first %s is a file name or "the N items", second %s is the system error. */
        report(1, _("Cannot move %s to Trash: %s."), what, why);
    }
    reload_all();
}

/* Shift+Delete (and Delete in trash://): the targets deleted, if the user
 * says so; in the Trash, with their .trashinfo. */
static void delete_targets(void)
{
    int idx[MAX_CLIP], n = targets(P, idx, MAX_CLIP), failed = 0;
    char question[512], what[300];
    if (n == 0) {
        return;
    }
    describe(P, idx, n, what, sizeof what);
    if (n == 1) {
        snprintf(question, sizeof question, _("Delete %s permanently?"), what);
    } else {
        snprintf(question, sizeof question,
                 P_("Delete the %d marked item permanently?", "Delete the %d marked items permanently?", n), n);
    }
    if (!yes(question)) {
        report(0, _("Nothing deleted."));
        return;
    }
    as_user(1);
    for (int i = 0; i < n; i++) {
        char path[PATH_MAX], file[PATH_MAX], info[PATH_MAX];
        const struct file *f = &P->list[idx[i]];
        if (item_path(P, idx[i], path, sizeof path) < 0 || remove_tree(path) != 0) {
            failed++;
        } else if (f->troot && trash_paths(f->troot, f->name, file, info, sizeof file) == 0) {
            unlink(info);
        }
    }
    as_user(0);
    if (failed) {
        report(1, P_("Deleted %d item; some could not be deleted.",
                     "Deleted %d items; some could not be deleted.", n - failed), n - failed);
    } else {
        report(0, P_("Deleted %d item.", "Deleted %d items.", n - failed), n - failed);
    }
    reload_all();
}

/* r in trash://: each target back where it was (the folders above it made
 * again), never over what is there now. */
static void restore_targets(void)
{
    int idx[MAX_CLIP], n = targets(P, idx, MAX_CLIP), done = 0;
    char why[PATH_MAX + 64] = "";
    as_user(1);
    for (int i = 0; i < n; i++) {
        const struct file *f = &P->list[idx[i]];
        char file[PATH_MAX], info[PATH_MAX], up[PATH_MAX];
        struct stat st;
        if (!f->troot || trash_paths(f->troot, f->name, file, info, sizeof file) < 0) {
            continue;
        }
        parent_of(f->origin, up, sizeof up);
        if (lstat(f->origin, &st) == 0) {
            snprintf(why, sizeof why, _("%s is there already: not restored"), f->origin);
        } else if (mkdir_p(up, 0755) < 0 || move_path(file, f->origin) < 0) {
            /* TRANSLATORS: first %s is a path, second %s is the system error. */
            snprintf(why, sizeof why, _("cannot restore %s: %s"), f->origin, strerror(errno));
        } else {
            unlink(info);
            done++;
        }
    }
    as_user(0);
    if (why[0]) {
        /* TRANSLATORS: %d is the count restored, %s is why another item could not be restored. */
        report(1, P_("Restored %d item; %s.", "Restored %d items; %s.", done), done, why);
    } else {
        report(0, P_("Restored %d item to where it was.", "Restored %d items to where they were.", done), done);
    }
    reload_all();
}

/* e in trash://: everything in every trash directory deleted, if the user
 * says so. */
static void empty_trash(void)
{
    char roots[16][PATH_MAX], question[128];
    int count = 0, failed = 0;
    as_user(1);
    int n = trash_roots(roots, 16);
    for (int r = 0; r < n; r++) {
        char infos[PATH_MAX];
        DIR *d = join(infos, sizeof infos, roots[r], "info") == 0 ? opendir(infos) : NULL;
        struct dirent *e;
        while (d && (e = readdir(d)) != NULL) {
            count += e->d_name[0] != '.' || (strcmp(e->d_name, ".") && strcmp(e->d_name, ".."));
        }
        if (d) closedir(d);
    }
    as_user(0);
    if (count == 0) {
        report(0, _("The Trash is empty."));
        return;
    }
    snprintf(question, sizeof question,
             P_("Empty the Trash: delete its %d item permanently?",
                "Empty the Trash: delete its %d items permanently?", count),
             count);
    if (!yes(question)) {
        report(0, _("Nothing deleted."));
        return;
    }
    as_user(1);
    for (int r = 0; r < n; r++) {
        for (int k = 0; k < 2; k++) {
            char sub[PATH_MAX];
            DIR *d = join(sub, sizeof sub, roots[r], k ? "info" : "files") == 0 ? opendir(sub) : NULL;
            struct dirent *e;
            while (d && (e = readdir(d)) != NULL) {
                char path[PATH_MAX];
                if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0) continue;
                failed += join(path, sizeof path, sub, e->d_name) < 0 || remove_tree(path) != 0;
            }
            if (d) closedir(d);
        }
    }
    as_user(0);
    report(failed != 0,
           failed ? _("The Trash is emptied, but for what could not be deleted.") : _("The Trash is empty."));
    reload_all();
}

/* Dropped (a drag of items): moved (copy: copied) into dir, or into the
 * Trash for TRASH_URL. */
static void drop(struct pane *from, const int *idx, int n, const char *dir, int copy)
{
    static char paths[MAX_CLIP][PATH_MAX];
    char *list[MAX_CLIP];
    if (pick_type || n == 0) {
        return;
    }
    if (strcmp(dir, TRASH_URL) == 0) {
        if (!is_trash(from)) trash_items(from, idx, n);
        return;
    }
    if (is_trash(from)) {
        /* TRANSLATORS: r is the key; keep it. */
        report(1, _("Items in the Trash are restored (r), not moved."));
        return;
    }
    int k = 0;
    for (int i = 0; i < n && k < MAX_CLIP; i++) {
        if (item_path(from, idx[i], paths[k], PATH_MAX) == 0) {
            list[k] = paths[k];
            k++;
        }
    }
    transfer(list, k, dir, !copy);
    reload_all();
}

static void new_folder(void)
{
    if (is_trash(P)) {
        report(1, _("No folder is made in the Trash."));
        return;
    }
    const char *name = prompt(_("New folder:"), NULL, 0);
    if (name && name[0] && !strchr(name, '/') && strcmp(name, ".") && strcmp(name, "..")) {
        char path[PATH_MAX], keep[256];
        snprintf(keep, sizeof keep, "%s", name);
        as_user(1);
        int ok = join(path, sizeof path, P->cwd, keep) == 0 && mkdir(path, 0755) == 0;
        as_user(0);
        if (ok) {
            struct undo *u = undo_begin(UNDO_MKDIR);
            undo_add(u, NULL, path, NULL);
            undo_end(u);
        }
        report(!ok, ok ? _("Made %s.") : _("Cannot make %s."), keep);
        load_dir(keep);
    }
}

static void rename_selected(void)
{
    char old[256];
    if (is_trash(P)) {
        /* TRANSLATORS: r is the key; keep it. */
        report(1, _("Items in the Trash are restored (r), not renamed."));
        return;
    }
    snprintf(old, sizeof old, "%s", P->list[P->sel].name);
    const char *name = prompt(_("Rename to:"), old, 0);
    if (name && name[0] && !strchr(name, '/') && strcmp(name, old) != 0) {
        char a[PATH_MAX], b[PATH_MAX], keep[256];
        struct stat st;
        snprintf(keep, sizeof keep, "%s", name);
        as_user(1);
        int ok = join(a, sizeof a, P->cwd, old) == 0 && join(b, sizeof b, P->cwd, keep) == 0 &&
                 lstat(b, &st) != 0 && rename(a, b) == 0;
        as_user(0);
        if (ok) {
            struct undo *u = undo_begin(UNDO_RENAME);
            undo_add(u, a, b, NULL);
            undo_end(u);
        }
        /* TRANSLATORS: %s is the new name; Ctrl+Z is the key; keep it. */
        report(!ok, ok ? _("Renamed to %s (Ctrl+Z to undo).") : _("Cannot rename to %s (taken?)."), keep);
        load_dir(ok ? keep : old);
    }
}

static void new_empty_file(void)
{
    if (is_trash(P) || is_search(P)) {
        report(1, _("No file is created here."));
        return;
    }
    const char *name = prompt(_("New empty file:"), NULL, 0);
    if (name && name[0] && !strchr(name, '/') && strcmp(name, ".") && strcmp(name, "..")) {
        char path[PATH_MAX], keep[256];
        snprintf(keep, sizeof keep, "%s", name);
        as_user(1);
        int fd = join(path, sizeof path, P->cwd, keep) == 0 ?
                 open(path, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0644) : -1;
        if (fd >= 0) close(fd);
        as_user(0);
        if (fd >= 0) {
            struct undo *u = undo_begin(UNDO_CREATE);
            undo_add(u, NULL, path, NULL);
            undo_end(u);
        }
        report(fd < 0, fd >= 0 ? _("Created %s (Ctrl+Z to undo).") : _("Cannot create %s."), keep);
        load_dir(keep);
    }
}

static void create_symlink(void)
{
    if (is_trash(P) || is_search(P) || P->nlist == 0) {
        return;
    }
    char target[PATH_MAX], def_name[280];
    if (item_path(P, P->sel, target, sizeof target) < 0) {
        return;
    }
    snprintf(def_name, sizeof def_name, "%.240s.link", P->list[P->sel].name);
    const char *name = prompt(_("Symbolic link name:"), def_name, 1);
    if (name && name[0] && !strchr(name, '/') && strcmp(name, ".") && strcmp(name, "..")) {
        char linkp[PATH_MAX], keep[256];
        snprintf(keep, sizeof keep, "%s", name);
        as_user(1);
        int ok = join(linkp, sizeof linkp, P->cwd, keep) == 0 && symlink(target, linkp) == 0;
        as_user(0);
        if (ok) {
            struct undo *u = undo_begin(UNDO_CREATE);
            undo_add(u, NULL, linkp, NULL);
            undo_end(u);
        }
        report(!ok, ok ? _("Created link %s (Ctrl+Z to undo).") : _("Cannot create link %s."), keep);
        load_dir(keep);
    }
}

static void duplicate_selected(void)
{
    if (is_trash(P) || is_search(P) || P->nlist == 0) {
        return;
    }
    static char paths[MAX_CLIP][PATH_MAX];
    char *list[MAX_CLIP];
    int idx[MAX_CLIP], n = targets(P, idx, MAX_CLIP), k = 0;
    for (int i = 0; i < n && k < MAX_CLIP; i++) {
        if (item_path(P, idx[i], paths[k], PATH_MAX) == 0) {
            list[k] = paths[k];
            k++;
        }
    }
    if (k > 0) {
        transfer(list, k, P->cwd, 0);
        reload_all();
    }
}

static void copy_paths(void)
{
    int idx[MAX_CLIP], n = targets(P, idx, MAX_CLIP);
    static char buf[MAX_CLIP * 512];
    size_t len = 0;
    if (n == 0) {
        if (!is_trash(P) && !is_search(P)) {
            len = (size_t)snprintf(buf, sizeof buf, "%s", P->cwd);
        }
    } else {
        for (int i = 0; i < n && len + PATH_MAX + 2 < sizeof buf; i++) {
            char path[PATH_MAX];
            if (item_path(P, idx[i], path, sizeof path) == 0) {
                len += (size_t)snprintf(buf + len, sizeof buf - len, "%s%s", i ? "\n" : "", path);
            }
        }
    }
    if (len > 0 && clipboard_set(CLIPBOARD_REGULAR, buf, len) == CLIPBOARD_OK) {
        report(0, P_("Copied %d path to the clipboard.", "Copied %d paths to the clipboard.", n ? n : 1), n ? n : 1);
    } else {
        report(1, _("Cannot copy path to the clipboard."));
    }
}

static void invert_selection(void)
{
    int count = 0;
    for (int i = 0; i < P->nlist; i++) {
        P->list[i].marked = !P->list[i].marked;
        count += P->list[i].marked;
    }
    report(0, P_("Inverted selection: %d item marked.", "Inverted selection: %d items marked.", count), count);
}

static void open_terminal_here(void)
{
    const char *dir = is_search(P) && P->search_root[0] ? P->search_root : is_trash(P) ? home : P->cwd;
    char why[256] = "";
    char *argv[] = {
        "/usr/local/bin/term", "/bin/sh", "-c",
        "cd -- \"$1\" 2>/dev/null || true; exec /bin/bash -l", "sh", (char *)dir, NULL
    };
    base_open(argv, why, sizeof why);
    report(0, _("Terminal closed (%s)."), base_name(dir));
}

static void find_files(void)
{
    const char *base_dir = is_search(P) && P->search_root[0] ? P->search_root : is_trash(P) ? home : P->cwd;
    char root_copy[PATH_MAX];
    snprintf(root_copy, sizeof root_copy, "%s", base_dir);
    const char *q = prompt(_("Find files (part of name or *glob*):"), P->search_query, 1);
    if (!q || !q[0]) {
        return;
    }
    snprintf(P->search_root, sizeof P->search_root, "%s", root_copy);
    snprintf(P->search_query, sizeof P->search_query, "%s", q);
    P->view = VIEW_DETAILS;
    go(SEARCH_URL, NULL);
    report(0, P_("Found %d match in %s.", "Found %d matches in %s.", P->nlist), P->nlist, base_name(root_copy));
}

static void prefs_path(char *out, size_t size)
{
    snprintf(out, size, "%s/.config/locachy/files.conf", home);
}

static void load_prefs(void)
{
    char path[PATH_MAX], line[256];
    if (!home[0]) return;
    prefs_path(path, sizeof path);
    as_user(1);
    FILE *f = fopen(path, "re");
    as_user(0);
    if (!f) return;
    while (fgets(line, sizeof line, f)) {
        line[strcspn(line, "\r\n")] = '\0';
        char *eq = strchr(line, '=');
        if (!eq) continue;
        *eq++ = '\0';
        int v = atoi(eq);
        if (strcmp(line, "cols_mask") == 0) cols_mask = v & 0x3F;
        else if (strcmp(line, "mingle_dirs") == 0) mingle_dirs = v != 0;
        else if (strcmp(line, "sort_case") == 0) sort_case = v != 0;
        else if (strcmp(line, "side_hidden") == 0) side_hidden = v != 0;
        else if (strcmp(line, "view") == 0 && ntabs > 0) tabs[0].pane[0].view = clampi(v, 0, VIEWS - 1);
        else if (strcmp(line, "sort") == 0 && ntabs > 0) tabs[0].pane[0].sort = clampi(v, 0, SORT_OWNER);
        else if (strcmp(line, "desc") == 0 && ntabs > 0) tabs[0].pane[0].desc = v != 0;
    }
    fclose(f);
}

static void save_prefs(void)
{
    char dir[PATH_MAX], path[PATH_MAX], tmp[PATH_MAX + 8];
    if (!home[0]) return;
    snprintf(dir, sizeof dir, "%s/.config/locachy", home);
    prefs_path(path, sizeof path);
    snprintf(tmp, sizeof tmp, "%s.new", path);
    as_user(1);
    if (mkdir_p(dir, 0700) == 0) {
        int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
        if (fd >= 0) {
            dprintf(fd, "view=%d\nsort=%d\ndesc=%d\ncols_mask=%d\nmingle_dirs=%d\nsort_case=%d\nside_hidden=%d\n",
                    P ? P->view : VIEW_DETAILS, P ? P->sort : SORT_NAME, P ? P->desc : 0,
                    cols_mask, mingle_dirs, sort_case, side_hidden);
            if (close(fd) == 0) rename(tmp, path);
            else unlink(tmp);
        }
    }
    as_user(0);
}

static void bookmarks_path(char *out, size_t size)
{
    snprintf(out, size, "%s/.config/gtk-3.0/bookmarks", home);
}

static void rebuild_places(void)
{
    nplaces = 0;
    if (!home[0]) return;
    snprintf(places[nplaces++], PATH_MAX, "%s", home);
    char copy[256];
    snprintf(copy, sizeof copy, "%s", dirs);
    for (char *d = strtok(copy, " "); d && nplaces < MAX_PLACES - 1; d = strtok(NULL, " ")) {
        snprintf(places[nplaces++], PATH_MAX, "%s/%s", home, d);
    }
    char bpath[PATH_MAX], line[PATH_MAX + 128];
    bookmarks_path(bpath, sizeof bpath);
    as_user(1);
    FILE *bf = fopen(bpath, "re");
    as_user(0);
    while (bf && fgets(line, sizeof line, bf) && nplaces < MAX_PLACES - 1) {
        line[strcspn(line, "\r\n")] = '\0';
        char *sp = strchr(line, ' ');
        if (sp) *sp = '\0';
        if (strncmp(line, "file://", 7) != 0) continue;
        char dec[PATH_MAX];
        url_unescape(line + 7, dec, sizeof dec);
        if (dec[0] != '/') continue;
        int dup = 0;
        for (int i = 0; i < nplaces; i++) {
            if (strcmp(places[i], dec) == 0) { dup = 1; break; }
        }
        if (!dup) snprintf(places[nplaces++], PATH_MAX, "%s", dec);
    }
    if (bf) fclose(bf);
    snprintf(places[nplaces++], PATH_MAX, "%s", TRASH_URL);
    if (side_sel >= nplaces) side_sel = nplaces - 1;
}

static void toggle_bookmark(void)
{
    if (is_trash(P) || is_search(P) || !home[0]) {
        report(1, _("Only a folder on disk can be bookmarked."));
        return;
    }
    char dir[PATH_MAX], bpath[PATH_MAX], tmp[PATH_MAX + 8], line[PATH_MAX + 128];
    static char kept[MAX_PLACES][PATH_MAX + 128];
    int nkept = 0, removed = 0;
    snprintf(dir, sizeof dir, "%s/.config/gtk-3.0", home);
    bookmarks_path(bpath, sizeof bpath);
    snprintf(tmp, sizeof tmp, "%s.new", bpath);
    as_user(1);
    FILE *bf = fopen(bpath, "re");
    while (bf && fgets(line, sizeof line, bf) && nkept < MAX_PLACES) {
        line[strcspn(line, "\r\n")] = '\0';
        if (!line[0]) continue;
        char uri[PATH_MAX + 128], dec[PATH_MAX];
        snprintf(uri, sizeof uri, "%s", line);
        char *sp = strchr(uri, ' ');
        if (sp) *sp = '\0';
        if (strncmp(uri, "file://", 7) == 0) {
            url_unescape(uri + 7, dec, sizeof dec);
            if (strcmp(dec, P->cwd) == 0) {
                removed = 1;
                continue;
            }
        }
        snprintf(kept[nkept++], sizeof kept[0], "%s", line);
    }
    if (bf) fclose(bf);
    int ok = mkdir_p(dir, 0700) == 0;
    if (ok) {
        int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
        if (fd >= 0) {
            for (int i = 0; i < nkept; i++) dprintf(fd, "%s\n", kept[i]);
            if (!removed) {
                char esc[PATH_MAX * 3];
                url_escape(P->cwd, esc, sizeof esc);
                dprintf(fd, "file://%s\n", esc);
            }
            ok = close(fd) == 0 && rename(tmp, bpath) == 0;
            if (!ok) unlink(tmp);
        } else {
            ok = 0;
        }
    }
    as_user(0);
    rebuild_places();
    if (!ok) {
        report(1, _("Cannot update bookmarks."));
    } else if (removed) {
        report(0, _("Removed bookmark: %s."), base_name(P->cwd));
    } else {
        /* TRANSLATORS: Ctrl+D is the key; keep it. */
        report(0, _("Bookmarked %s (Ctrl+D removes it)."), base_name(P->cwd));
    }
}

/*
 * The screen's layout:
 *   row 0:   screen_bar ("files")
 *   row 1:   [<] [>] [^]  location (breadcrumbs, or trash://)  [Icons][Compact][Details]
 *   row 2:   the tabs, when there are two or more
 *   rows:    places at the left (SIDE columns); one pane, or two with a
 *            divider column between them; a pane in Details view has its
 *            column header on its top row
 *   rows-3:  the status message (or a prompt, or a question)
 *   rows-2, rows-1: the buttons
 */
static int cell_w, cell_h, top_px;

static void cell_metrics(void)
{
    cell_w = cell_h = top_px = 0;
    screen_cells(&cell_w, &cell_h, &top_px);
    if (cell_w <= 0) cell_w = 8;
    if (cell_h <= 0) cell_h = 16;
}

static int body_top(void)
{
    return ntabs > 1 ? 3 : 2;
}

static int rows(void)
{
    int r = screen_rows() - 3 - body_top();
    return r > 1 ? r : 1;
}

static void pane_cols(int which, int *col, int *cols)
{
    int sw = side_width();
    int w = screen_cols() - sw;
    if (w < 4) w = 4;
    if (!tabs[cur_tab].split) {
        *col = sw;
        *cols = w;
    } else if (which == 0) {
        *col = sw;
        *cols = (w - 1) / 2;
    } else {
        int left = (w - 1) / 2;
        *col = sw + left + 1;
        *cols = w - 1 - left;
    }
    if (*cols < 2) *cols = 2;
}

static void pane_area(const struct pane *p, int which, int *col, int *row, int *cols, int *nrows)
{
    pane_cols(which, col, cols);
    int head = p->view == VIEW_DETAILS && rows() > 2 ? 1 : 0;
    *row = body_top() + head;
    *nrows = rows() - head;
    if (*nrows < 1) *nrows = 1;
}

/* The grid of a pane: an item is tile_cols by tile_rows cells, grid_cols of
 * them across, view_rows rows of tiles on the screen at once. */
static void pane_grid(const struct pane *p, int cols, int nrows, int *grid_cols, int *tile_cols, int *tile_rows,
                      int *view_rows)
{
    int z = clampi(p->zoom[p->view], 0, zoom_max[p->view]);
    if (p->view == VIEW_ICONS) {
        static const int tw[] = { 10, 12, 14, 16, 20, 24, 28 }, th[] = { 3, 3, 4, 4, 5, 6, 7 };
        *tile_cols = tw[z] < cols ? tw[z] : cols;
        *tile_rows = th[z];
    } else if (p->view == VIEW_COMPACT) {
        static const int tw[] = { 20, 26, 34 };
        *tile_cols = tw[z] < cols ? tw[z] : cols;
        *tile_rows = z == 2 ? 2 : 1;
    } else {
        *tile_cols = cols;
        *tile_rows = 1 + z;
    }
    *grid_cols = cols / *tile_cols;
    if (*grid_cols < 1) *grid_cols = 1;
    if (p->view == VIEW_DETAILS) *grid_cols = 1;
    *view_rows = nrows / *tile_rows;
    if (*view_rows < 1) *view_rows = 1;
}

static void clamp_scroll(struct pane *p, int which)
{
    int col, row, cols, nrows, gc, tc, tr, vr;
    pane_area(p, which, &col, &row, &cols, &nrows);
    pane_grid(p, cols, nrows, &gc, &tc, &tr, &vr);
    p->sel = clampi(p->sel, 0, p->nlist > 0 ? p->nlist - 1 : 0);
    int sr = p->sel / gc, tr0 = p->top / gc;
    if (drag.mode != DRAG_BAND || drag.pane != which) {
        if (sr < tr0) tr0 = sr;
        if (sr >= tr0 + vr) tr0 = sr - vr + 1;
    }
    int max_row = p->nlist > 0 ? (p->nlist - 1) / gc : 0;
    if (tr0 > max_row) tr0 = max_row;
    if (tr0 < 0) tr0 = 0;
    p->top = tr0 * gc;
}

/* What kind of icon an item gets, from its name (and dir/exec). */
static int item_kind(const struct file *f)
{
    if (f->dir) return KIND_FOLDER;
    if (f->exec) return KIND_EXEC;
    const char *dot = strrchr(shown_name(f), '.'), *ext = dot ? dot + 1 : "";
    if (!strcasecmp(ext, "png") || !strcasecmp(ext, "jpg") || !strcasecmp(ext, "jpeg") || !strcasecmp(ext, "gif") ||
        !strcasecmp(ext, "webp") || !strcasecmp(ext, "svg") || !strcasecmp(ext, "bmp") || !strcasecmp(ext, "jxl") ||
        !strcasecmp(ext, "heic") || !strcasecmp(ext, "avif") || !strcasecmp(ext, "xcf")) {
        return KIND_IMAGE;
    }
    if (!strcasecmp(ext, "pdf") || !strcasecmp(ext, "epub") || !strcasecmp(ext, "djvu") || !strcasecmp(ext, "ps")) {
        return KIND_PDF;
    }
    if (!strcasecmp(ext, "zip") || !strcasecmp(ext, "tar") || !strcasecmp(ext, "gz") || !strcasecmp(ext, "xz") ||
        !strcasecmp(ext, "zst") || !strcasecmp(ext, "bz2") || !strcasecmp(ext, "7z") || !strcasecmp(ext, "rar") ||
        !strcasecmp(ext, "iso")) {
        return KIND_ARCHIVE;
    }
    if (!strcasecmp(ext, "mp3") || !strcasecmp(ext, "flac") || !strcasecmp(ext, "ogg") || !strcasecmp(ext, "opus") ||
        !strcasecmp(ext, "wav") || !strcasecmp(ext, "m4a")) {
        return KIND_AUDIO;
    }
    if (!strcasecmp(ext, "mp4") || !strcasecmp(ext, "mkv") || !strcasecmp(ext, "webm") || !strcasecmp(ext, "mov") ||
        !strcasecmp(ext, "avi")) {
        return KIND_VIDEO;
    }
    if (!strcasecmp(ext, "txt") || !strcasecmp(ext, "md") || !strcasecmp(ext, "c") || !strcasecmp(ext, "h") ||
        !strcasecmp(ext, "py") || !strcasecmp(ext, "sh") || !strcasecmp(ext, "js") || !strcasecmp(ext, "ts") ||
        !strcasecmp(ext, "rs") || !strcasecmp(ext, "go") || !strcasecmp(ext, "json") || !strcasecmp(ext, "toml") ||
        !strcasecmp(ext, "yaml") || !strcasecmp(ext, "yml") || !strcasecmp(ext, "xml") || !strcasecmp(ext, "html") ||
        !strcasecmp(ext, "css") || !strcasecmp(ext, "conf") || !strcasecmp(ext, "ini")) {
        return KIND_TEXT;
    }
    return KIND_FILE;
}

/* The 1 pixel frame of (x, y, w, h). */
static void frame_px(int x, int y, int w, int h, uint32_t rgb)
{
    if (w <= 0 || h <= 0) return;
    screen_fill_px(x, y, w, 1, rgb);
    screen_fill_px(x, y + h - 1, w, 1, rgb);
    screen_fill_px(x, y, 1, h, rgb);
    screen_fill_px(x + w - 1, y, 1, h, rgb);
}

/* An icon drawn in the cell box (col, row, cols, rows): a folder with its
 * tab, or a page with its folded corner and a badge of its kind. */
static void draw_icon(int col, int row, int cols, int rows_box, const struct file *f, uint32_t bg)
{
    static const uint32_t ink[] = {
        [KIND_FOLDER] = 0x58A8F0, [KIND_FILE] = 0x90A0B0,  [KIND_TEXT] = 0x78B890,
        [KIND_IMAGE] = 0x68C088,  [KIND_AUDIO] = 0xB088E8, [KIND_VIDEO] = 0xE088B8,
        [KIND_PDF] = 0xE06868,    [KIND_ARCHIVE] = 0xE0A850,[KIND_EXEC] = 0x70D070,
    };
    int kind = item_kind(f);
    uint32_t c = ink[kind];
    screen_fill(col, row, cols, rows_box, bg);
    int bx = col * cell_w, by = top_px + row * cell_h, bw = cols * cell_w, bh = rows_box * cell_h;
    int h = bh - 4, w = h * 5 / 4;
    if (w > bw - 4) {
        w = bw - 4;
        h = w * 4 / 5;
    }
    if (w < 6 || h < 6) return;
    if (kind == KIND_FOLDER) {
        int x = bx + (bw - w) / 2, y = by + (bh - h) / 2, tab_h = h / 5 > 2 ? h / 5 : 2;
        screen_fill_px(x, y, w * 2 / 5, tab_h, 0x3C7CC0);
        screen_fill_px(x, y + tab_h, w, h - tab_h, c);
        screen_fill_px(x + 1, y + tab_h + 1, w - 2, 1, 0xA8D8FF);
        frame_px(x, y + tab_h, w, h - tab_h, 0x285888);
    } else {
        int pw = h * 4 / 5 < bw - 4 ? h * 4 / 5 : bw - 4;
        int x = bx + (bw - pw) / 2, y = by + (bh - h) / 2, fold = pw / 4 > 2 ? pw / 4 : 2;
        screen_fill_px(x, y, pw, h, 0xD8E0E8);
        frame_px(x, y, pw, h, 0x506070);
        screen_fill_px(x + pw - fold, y, fold, fold, bg);
        screen_fill_px(x + pw - fold, y + fold - 1, fold, 1, 0x90A0B0);
        screen_fill_px(x + pw - fold, y, 1, fold, 0x90A0B0);
        int band = h / 4 > 3 ? h / 4 : 3;
        screen_fill_px(x + 1, y + h - band - 1, pw - 2, band, c);
        for (int l = 0; kind == KIND_TEXT && l < 3 && y + 4 + l * 3 < y + h - band - 2; l++) {
            screen_fill_px(x + 3, y + 4 + l * 3, pw - fold - 4, 1, 0x607080);
        }
        if (kind == KIND_IMAGE && h > 12) {
            screen_fill_px(x + 3, y + 3, pw - fold - 3, h - band - 5, 0xB8E0C8);
        }
    }
    if (f->marked) {
        screen_text(col, row, "*", ACCENT, bg);
    } else if (f->link) {
        screen_text(col + cols - 1, row + rows_box - 1, "@", ACCENT, bg);
    }
}

struct details_cols {
    int name_w, size_w, mtime_w, ext_w, perm_w, owner_w, type_w;
};

static void compute_details_cols(int cols, struct details_cols *dc)
{
    memset(dc, 0, sizeof *dc);
    int rem = cols - 3;
    if ((cols_mask & COL_SIZE) && rem >= 24)  { dc->size_w = 12; rem -= 12; }
    if ((cols_mask & COL_MTIME) && rem >= 28) { dc->mtime_w = 16; rem -= 16; }
    if ((cols_mask & COL_EXT) && rem >= 22)   { dc->ext_w = 9; rem -= 9; }
    if ((cols_mask & COL_PERM) && rem >= 24)  { dc->perm_w = 12; rem -= 12; }
    if ((cols_mask & COL_OWNER) && rem >= 26) { dc->owner_w = 14; rem -= 14; }
    if ((cols_mask & COL_TYPE) && rem >= 24) {
        int tw = rem - 20 > 24 ? 24 : rem - 20;
        if (tw >= 8) { dc->type_w = tw; rem -= tw + 2; }
    }
    dc->name_w = rem >= 6 ? rem : (cols > 3 ? cols - 3 : 1);
}

/* The Details view's header: left-click sorts (again reverses), right-click
 * opens the Column Settings chooser. */
static void draw_details_header(const struct pane *p, int which, int col, int row, int cols,
                                const struct details_cols *dc)
{
    int sk = sort_key(p), trash = is_trash(p), search = is_search(p), base = HIT_SORT + which * 16;
    const char *arrow = p->desc ? " v" : " ^";
    char buf[256], label[128], cut[64];
    screen_fill(col, row, cols, 1, BAR_BG);
    snprintf(label, sizeof label, "  %s%s", _("Name"), sk == SORT_NAME ? arrow : "");
    pad(buf, sizeof buf, label, dc->name_w + 3);
    screen_text(col, row, buf, sk == SORT_NAME ? ACCENT : FG, BAR_BG);
    ui_hit(col, row, dc->name_w + 3, 1, base + SORT_NAME);
    int c = col + dc->name_w + 3;
    if (dc->size_w && c + dc->size_w <= col + cols) {
        snprintf(label, sizeof label, "%s%s", _("Size"), sk == SORT_SIZE ? arrow : "");
        fit(cut, sizeof cut, label, 10);
        int pad_left = clampi(10 - ui_cells(cut), 0, 10);
        snprintf(buf, sizeof buf, "%*s%.40s  ", pad_left, "", cut);
        screen_text(c, row, buf, sk == SORT_SIZE ? ACCENT : FG, BAR_BG);
        ui_hit(c, row, dc->size_w, 1, base + SORT_SIZE);
        c += dc->size_w;
    }
    if (dc->mtime_w && c + dc->mtime_w <= col + cols) {
        int dkey = trash ? SORT_DELETED : SORT_MTIME;
        snprintf(label, sizeof label, "%s%s", trash ? _("Deleted") : _("Modified"), sk == dkey ? arrow : "");
        pad(buf, sizeof buf, label, dc->mtime_w);
        screen_text(c, row, buf, sk == dkey ? ACCENT : FG, BAR_BG);
        ui_hit(c, row, dc->mtime_w, 1, base + SORT_MTIME);
        c += dc->mtime_w;
    }
    if (dc->ext_w && c + dc->ext_w <= col + cols) {
        snprintf(label, sizeof label, " %s%s", _("Ext"), sk == SORT_EXT ? arrow : "");
        pad(buf, sizeof buf, label, dc->ext_w);
        screen_text(c, row, buf, sk == SORT_EXT ? ACCENT : FG, BAR_BG);
        ui_hit(c, row, dc->ext_w, 1, base + SORT_EXT);
        c += dc->ext_w;
    }
    if (dc->perm_w && c + dc->perm_w <= col + cols) {
        snprintf(label, sizeof label, " %s%s", _("Perms"), sk == SORT_PERM ? arrow : "");
        pad(buf, sizeof buf, label, dc->perm_w);
        screen_text(c, row, buf, sk == SORT_PERM ? ACCENT : FG, BAR_BG);
        ui_hit(c, row, dc->perm_w, 1, base + SORT_PERM);
        c += dc->perm_w;
    }
    if (dc->owner_w && c + dc->owner_w <= col + cols) {
        snprintf(label, sizeof label, " %s%s", _("Owner"), sk == SORT_OWNER ? arrow : "");
        pad(buf, sizeof buf, label, dc->owner_w);
        screen_text(c, row, buf, sk == SORT_OWNER ? ACCENT : FG, BAR_BG);
        ui_hit(c, row, dc->owner_w, 1, base + SORT_OWNER);
        c += dc->owner_w;
    }
    if (dc->type_w > 0 && c + 2 + dc->type_w <= col + cols) {
        int tkey = (trash || search) ? SORT_ORIGIN : SORT_TYPE;
        const char *tname = trash ? _("Original Location") : search ? _("Location") : _("Type");
        snprintf(label, sizeof label, "%s%s", tname, sk == tkey ? arrow : "");
        pad(buf, sizeof buf, label, dc->type_w);
        screen_text(c + 2, row, buf, sk == tkey ? ACCENT : FG, BAR_BG);
        ui_hit(c + 2, row, dc->type_w, 1, base + SORT_TYPE);
    }
}

static void relative_search_dir(const struct pane *p, const struct file *f, char *out, size_t size)
{
    if (!f->origin) {
        out[0] = '\0';
        return;
    }
    char dir[PATH_MAX];
    parent_of(f->origin, dir, sizeof dir);
    size_t rlen = strlen(p->search_root);
    if (rlen > 1 && strncmp(dir, p->search_root, rlen) == 0) {
        const char *sub = dir + rlen;
        snprintf(out, size, "%s", *sub == '/' ? sub + 1 : (*sub ? sub : "."));
    } else {
        snprintf(out, size, "%s", dir);
    }
}

static void draw_pane(struct pane *p, int which)
{
    int active = !side && tabs[cur_tab].active == which;
    uint32_t pane_bg = tabs[cur_tab].split && tabs[cur_tab].active != which ? PANE_BG : BG;
    int col, row, cols, nrows, gc, tc, tr, vr;
    pane_area(p, which, &col, &row, &cols, &nrows);
    pane_grid(p, cols, nrows, &gc, &tc, &tr, &vr);
    clamp_scroll(p, which);
    screen_fill(col, body_top(), cols, rows(), pane_bg);
    ui_hit(col, row, cols, nrows, HIT_AREA + which);

    struct details_cols dc;
    compute_details_cols(cols, &dc);
    if (p->view == VIEW_DETAILS && rows() > 2) {
        draw_details_header(p, which, col, body_top(), cols, &dc);
    }
    if (p->nlist == 0) {
        screen_text(col + 1, row, p->filter[0] ? _("(nothing matches the filter)") :
                    is_trash(p) ? _("(Trash is empty)") :
                    is_search(p) ? _("(no matching files found)") : _("(empty)"), DIM, pane_bg);
        return;
    }

    int hit_base = HIT_FILE + which * HIT_PANE;
    for (int i = p->top; i < p->nlist; i++) {
        int k = i - p->top, r = k / gc, c = k % gc;
        if (r >= vr) break;
        struct file *f = &p->list[i];
        int ix = col + c * tc, iy = row + r * tr, tw = p->view == VIEW_DETAILS ? cols : tc;
        int on = active && i == p->sel;
        uint32_t bg = on ? ACCENT : f->marked ? MARK : pane_bg;
        uint32_t fg = on ? BG : f->dir ? ACCENT : f->exec ? 0x90E890 : FG;
        char label[300], cut[300], line[1024];
        snprintf(label, sizeof label, "%s%s", shown_name(f), f->dir ? "/" : "");
        screen_fill(ix, iy, tw, tr, bg);

        if (p->view == VIEW_ICONS) {
            draw_icon(ix + 1, iy, tw - 2, tr - 2, f, bg);
            size_t split = utf_fit(label, tw - 2);
            if (!label[split]) {
                int w = utf_cells(label, 0, split);
                screen_text(ix + (tw - w) / 2, iy + tr - 2, label, fg, bg);
            } else {
                char first[300];
                snprintf(first, sizeof first, "%.*s", (int)split, label);
                screen_text(ix + 1, iy + tr - 2, first, fg, bg);
                fit(cut, sizeof cut, label + split, tw - 2);
                int w = utf_cells(cut, 0, strlen(cut));
                screen_text(ix + (tw - w) / 2, iy + tr - 1, cut, fg, bg);
            }
        } else if (p->view == VIEW_COMPACT) {
            char mark = f->marked ? '*' : f->dir ? '/' : f->exec ? '*' : ' ';
            fit(cut, sizeof cut, label, tw - 4);
            snprintf(line, sizeof line, " %c %s", mark, cut);
            screen_text(ix, iy, line, fg, bg);
        } else {
            pad(cut, sizeof cut, label, dc.name_w);
            size_t len = (size_t)snprintf(line, sizeof line, "%c %s", f->marked ? '*' : ' ', cut);
            if (dc.size_w) {
                char size[32] = "", sfit[32];
                if (f->dir) {
                    int cnt = dir_items(p, f, i);
                    snprintf(size, sizeof size, P_("%d item", "%d items", cnt), cnt);
                } else {
                    human(f->size, size, sizeof size);
                }
                fit(sfit, sizeof sfit, size, 10);
                int pl = clampi(10 - ui_cells(sfit), 0, 10);
                len += (size_t)snprintf(line + len, sizeof line - len, " %*s%s ", pl, "", sfit);
            }
            if (dc.mtime_w) {
                char date[24] = "", dpad[32];
                time_t when = is_trash(p) && f->deleted ? f->deleted : f->mtime;
                strftime(date, sizeof date, "%Y-%m-%d %H:%M", localtime(&when));
                pad(dpad, sizeof dpad, date, dc.mtime_w);
                len += (size_t)snprintf(line + len, sizeof line - len, "%s", dpad);
            }
            if (dc.ext_w) {
                char epad[24];
                pad(epad, sizeof epad, file_ext(f), dc.ext_w - 1);
                len += (size_t)snprintf(line + len, sizeof line - len, " %s", epad);
            }
            if (dc.perm_w) {
                char perm[16], ppad[24];
                mode_string(f->mode, perm);
                pad(ppad, sizeof ppad, perm, dc.perm_w - 1);
                len += (size_t)snprintf(line + len, sizeof line - len, " %s", ppad);
            }
            if (dc.owner_w) {
                char own[64], opad[32];
                owner_string(f, own, sizeof own);
                pad(opad, sizeof opad, own, dc.owner_w - 1);
                len += (size_t)snprintf(line + len, sizeof line - len, " %s", opad);
            }
            if (dc.type_w > 0) {
                char rel[PATH_MAX], tpad[256];
                const char *t;
                if (is_trash(p)) {
                    t = f->origin ? f->origin : "";
                } else if (is_search(p)) {
                    relative_search_dir(p, f, rel, sizeof rel);
                    t = rel;
                } else {
                    t = item_type(p, i);
                }
                pad(tpad, sizeof tpad, t, dc.type_w);
                snprintf(line + len, sizeof line - len, "  %s", tpad);
            }
            screen_text(ix + 1, iy, line, fg, bg);
            if (tr > 1) {
                char rel[PATH_MAX];
                const char *sub;
                if (is_trash(p) && f->origin) sub = f->origin;
                else if (is_search(p)) { relative_search_dir(p, f, rel, sizeof rel); sub = rel; }
                else sub = item_type(p, i);
                fit(cut, sizeof cut, sub, cols - 4);
                screen_text(ix + 3, iy + 1, cut, on ? BG : DIM, bg);
            }
        }
        if (!active && i == p->sel) {
            frame_px(ix * cell_w, top_px + iy * cell_h, tw * cell_w, tr * cell_h, ACCENT);
        }
        ui_hit(ix, iy, tw, tr, hit_base + i);
    }
}

/* Row 1: Back, Forward, Up, the clickable path (from / down) or trash://,
 * and the three view buttons at the right. */
static void draw_toolbar(void)
{
    static const char *const vname[VIEWS] = { N_("Icons"), N_("Compact"), N_("Details") };
    int cols = screen_cols();
    screen_fill(0, 1, cols, 1, BAR_BG);
    int c = 1;
    screen_text(c, 1, "<", P->nback ? ACCENT : DIM, BAR_BG);
    ui_hit(c, 1, 1, 1, HIT_KEY + MODS(KEYS_ALT) + K_LEFT);
    screen_text(c + 2, 1, ">", P->nfwd ? ACCENT : DIM, BAR_BG);
    ui_hit(c + 2, 1, 1, 1, HIT_KEY + MODS(KEYS_ALT) + K_RIGHT);
    screen_text(c + 4, 1, "^", !is_trash(P) && strcmp(P->cwd, "/") != 0 ? ACCENT : DIM, BAR_BG);
    ui_hit(c + 4, 1, 1, 1, HIT_KEY + MODS(KEYS_ALT) + K_UP);
    c += 6;

    int right = cols;
    for (int v = VIEWS - 1; v >= 0 && right > c + 24; v--) {
        const char *label = _(vname[v]);
        int len = ui_cells(label) + 2, on = P->view == v;
        right -= len + 1;
        screen_fill(right, 1, len, 1, on ? ACCENT : BAR_BG);
        screen_text(right + 1, 1, label, on ? BG : FG, on ? ACCENT : BAR_BG);
        ui_hit(right, 1, len, 1, HIT_VIEW + v);
    }

    int path_col = c, max_path = right - path_col - 1;
    char raw[PATH_MAX + 512], shown_cwd[PATH_MAX + 256], line[1024];
    if (is_search(P)) {
        snprintf(shown_cwd, sizeof shown_cwd, _("Search '%s' in %s"), P->search_query, P->search_root);
    } else {
        snprintf(shown_cwd, sizeof shown_cwd, "%.800s", P->cwd);
    }
    /* TRANSLATORS: v is the key; keep it. */
    snprintf(raw, sizeof raw, "%s%s%s%s", shown_cwd, P->filter[0] ? _("   filter: ") : "", P->filter,
             nclip ? (clip_cut ? _("   (cut: v moves here)") : _("   (copied: v pastes here)")) : "");
    fit(line, sizeof line, raw, max_path);
    screen_text(path_col, 1, line, ACCENT, BAR_BG);
    if (!is_trash(P) && !is_search(P)) {
        ui_hit(path_col, 1, 1, 1, HIT_PATH + 0); /* / */
        int len = (int)strlen(P->cwd);
        for (int k = 1, start = 1; k <= len && path_col + start < right; k++) {
            if (P->cwd[k] == '/' || P->cwd[k] == '\0') {
                int w = k - start;
                if (w > 0 && path_col + start + w <= right) {
                    ui_hit(path_col + start, 1, w, 1, HIT_PATH + k);
                }
                start = k + 1;
            }
        }
    }
}

static void draw_tabs(void)
{
    int cols = screen_cols(), c = side_width();
    screen_fill(0, 2, cols, 1, BAR_BG);
    for (int t = 0; t < ntabs && c + 6 < cols; t++) {
        const struct pane *p = &tabs[t].pane[tabs[t].active];
        const char *title = is_trash(p) ? _("Trash") : is_search(p) ? _("Search") :
                            strcmp(p->cwd, home) == 0 ? _("Home") : base_name(p->cwd);
        char tab[64], cut[28];
        fit(cut, sizeof cut, title, 18);
        snprintf(tab, sizeof tab, " %d:%s%s ", t + 1, cut, tabs[t].split ? "|" : "");
        int w = utf_cells(tab, 0, strlen(tab)), on = t == cur_tab;
        if (c + w > cols) w = cols - c;
        screen_fill(c, 2, w, 1, on ? ACCENT : BAR_BG);
        screen_text(c, 2, tab, on ? BG : FG, on ? ACCENT : BAR_BG);
        ui_hit(c, 2, w, 1, HIT_TAB + t);
        c += w + 1;
    }
    if (c + 3 <= cols) {
        screen_text(c, 2, "[+]", ACCENT, BAR_BG);
        ui_hit(c, 2, 3, 1, HIT_KEY + MODS(KEYS_CTRL) + ('t' & 0x1F));
    }
}

/* The rubberband rectangle in pixel coordinates of the screen. */
static void draw_rubberband(void)
{
    struct pane *p = &tabs[cur_tab].pane[drag.pane];
    int col, row, cols, nrows, gc, tc, tr, vr;
    pane_area(p, drag.pane, &col, &row, &cols, &nrows);
    pane_grid(p, cols, nrows, &gc, &tc, &tr, &vr);
    int ox = col * cell_w, oy = top_px + row * cell_h, scroll = (p->top / gc) * tr * cell_h;
    int x0 = ox + drag.ax, y0 = oy + drag.ay - scroll, x1 = drag.x, y1 = drag.y;
    int rx = (x0 < x1 ? x0 : x1), ry = (y0 < y1 ? y0 : y1);
    int rw = abs(x1 - x0) + 1, rh = abs(y1 - y0) + 1;
    int min_x = ox, max_x = ox + cols * cell_w, min_y = oy, max_y = oy + nrows * cell_h;
    if (rx < min_x) { rw -= min_x - rx; rx = min_x; }
    if (ry < min_y) { rh -= min_y - ry; ry = min_y; }
    if (rx + rw > max_x) rw = max_x - rx;
    if (ry + rh > max_y) rh = max_y - ry;
    frame_px(rx, ry, rw, rh, ACCENT);
}

static void format_status_line(struct pane *p, char *out, size_t size)
{
    char left[320] = "", right[160] = "", free_str[32] = "";
    int nmark = 0;
    long long mark_bytes = 0;
    for (int i = 0; i < p->nlist; i++) {
        if (p->list[i].marked) {
            nmark++;
            if (!p->list[i].dir) mark_bytes += p->list[i].size;
        }
    }
    if (nmark > 0) {
        char mb[32];
        human(mark_bytes, mb, sizeof mb);
        snprintf(left, sizeof left, P_("%d selected (%s)", "%d selected (%s)", nmark), nmark, mb);
    } else if (p->nlist > 0 && p->sel < p->nlist) {
        struct file *f = &p->list[p->sel];
        char sz[32], dt[24], perm[16];
        if (f->dir) {
            int cnt = dir_items(p, f, p->sel);
            snprintf(sz, sizeof sz, P_("%d item", "%d items", cnt), cnt);
        } else {
            human(f->size, sz, sizeof sz);
        }
        time_t when = is_trash(p) && f->deleted ? f->deleted : f->mtime;
        strftime(dt, sizeof dt, "%Y-%m-%d %H:%M", localtime(&when));
        mode_string(f->mode, perm);
        snprintf(left, sizeof left, "%.100s — %s, %.48s, %s, %s",
                 shown_name(f), sz, item_type(p, p->sel), dt, perm);
    }

    const char *fs_dir = is_search(p) ? p->search_root : is_trash(p) ? home : p->cwd;
    struct statvfs vfs;
    if (fs_dir[0] && statvfs(fs_dir, &vfs) == 0 && vfs.f_blocks > 0) {
        human((long long)vfs.f_bavail * (long long)vfs.f_frsize, free_str, sizeof free_str);
    }
    if (p->nhidden > 0 && free_str[0]) {
        snprintf(right, sizeof right, P_("%d item (%d hidden) | %s free", "%d items (%d hidden) | %s free", p->nlist),
                 p->nlist, p->nhidden, free_str);
    } else if (free_str[0]) {
        snprintf(right, sizeof right, P_("%d item | %s free", "%d items | %s free", p->nlist), p->nlist, free_str);
    } else {
        snprintf(right, sizeof right, P_("%d item", "%d items", p->nlist), p->nlist);
    }

    int avail = screen_cols() - 2;
    if (avail < 10) {
        snprintf(out, size, "%s", left[0] ? left : right);
        return;
    }
    int rw = ui_cells(right);
    if (left[0] && rw + 4 < avail) {
        char lcut[320];
        pad(lcut, sizeof lcut, left, avail - rw - 2);
        snprintf(out, size, "%s  %s", lcut, right);
    } else {
        fit(out, size, left[0] ? left : right, avail);
    }
}

/* A button that leaves keep columns free at its right, or none (col). */
static int button_before(int col, int row, const char *label, int id, int keep)
{
    return col + ui_cells(label) + 2 + keep > screen_cols() ? col : ui_button(col, row, label, id);
}

static void draw(void)
{
    char line[256];
    int cols = screen_cols(), r = rows(), top_row = body_top();
    set_active();
    cell_metrics();
    screen_fill(0, 0, cols, screen_rows(), BG);
    ui_hits_clear();
    screen_bar("files", NULL);
    draw_toolbar();
    if (ntabs > 1) {
        draw_tabs();
    }

    if (!side_hidden) {
        for (int i = 0; i < nplaces && i < r; i++) {
            const char *name = strcmp(places[i], home) == 0 ? _("Home") :
                               strcmp(places[i], TRASH_URL) == 0 ? _("Trash") : base_name(places[i]);
            int on = side && i == side_sel, here = strcmp(places[i], P->cwd) == 0;
            char cut[64];
            screen_fill(0, top_row + i, SIDE, 1, on ? ACCENT : BG);
            snprintf(cut, sizeof cut, "%.*s", (int)utf_fit(name, SIDE - 5), name);
            snprintf(line, sizeof line, " %c %s", here ? '>' : ' ', cut);
            screen_text(0, top_row + i, line, on ? BG : here ? ACCENT : FG, on ? ACCENT : BG);
            ui_hit(0, top_row + i, SIDE, 1, HIT_PLACE + i);
        }
    }

    draw_pane(&tabs[cur_tab].pane[0], 0);
    if (tabs[cur_tab].split) {
        int col, w;
        pane_cols(0, &col, &w);
        screen_fill_px((col + w) * cell_w + cell_w / 2, top_px + top_row * cell_h, 1, r * cell_h, DIM);
        draw_pane(&tabs[cur_tab].pane[1], 1);
    }
    if (drag.mode == DRAG_BAND) {
        draw_rubberband();
    } else if (drag.mode == DRAG_ITEMS) {
        char tag[64];
        snprintf(tag, sizeof tag, " %s ", drag.mods & KEYS_CTRL ? _("+ copy") : _("-> move"));
        screen_text(drag.col + 1, drag.row, tag, BG, ACCENT);
    }

    if (msg_sticky && msg[0]) {
        screen_text(1, screen_rows() - 3, msg, error ? ERROR : ACCENT, BG);
    } else {
        char sline[512];
        format_status_line(P, sline, sizeof sline);
        screen_text(1, screen_rows() - 3, sline, DIM, BG);
    }
    int b2 = screen_rows() - 2, b1 = screen_rows() - 1, c;
    if (pick_type) {
        /* TRANSLATORS: o and Enter are keys; keep them. */
        c = ui_button(1, b2, picking_folder() ? _("o: choose this folder") : _("Enter: choose"),
                      HIT_KEY + (picking_folder() ? 'o' : '\n'));
        /* TRANSLATORS: / is the key; keep it. */
        c = ui_button(c, b2, _("/: filter"), HIT_KEY + '/');
        /* TRANSLATORS: . is the key; keep it. */
        c = ui_button(c, b2, _(".: hidden"), HIT_KEY + '.');
        /* TRANSLATORS: s is the key; keep it. */
        c = ui_button(c, b2, _("s: sort"), HIT_KEY + 's');
        /* TRANSLATORS: q is the key; keep it. */
        ui_button(c, b2, _("q: back, nothing chosen"), HIT_KEY + 'q');
        /* TRANSLATORS: Enter and Tab are keys; keep them. */
        screen_text(1, b1, picking_folder() ? _("Enter or a double click: go in   Tab: places") :
                    _("a double click: choose (a folder: go in)   Tab: places"), FG, BG);
        screen_flush();
        return;
    }
    if (is_trash(P)) {
        /* TRANSLATORS: r is the key; keep it. */
        c = ui_button(1, b2, _("r: restore"), HIT_KEY + 'r');
        /* TRANSLATORS: d is the key; keep it. */
        c = ui_button(c, b2, _("d: delete permanently"), HIT_KEY + 'd');
        /* TRANSLATORS: e is the key; keep it. */
        c = ui_button(c, b2, _("e: empty Trash"), HIT_KEY + 'e');
        /* TRANSLATORS: u is the key; keep it. */
        c = ui_button(c, b2, _("u: undo"), HIT_KEY + 'u');
        /* TRANSLATORS: i is the key; keep it. */
        c = ui_button(c, b2, _("i: properties"), HIT_KEY + 'i');
    } else {
        /* TRANSLATORS: Enter is the key; keep it. */
        c = ui_button(1, b2, _("Enter: open"), HIT_KEY + '\n');
        /* TRANSLATORS: o is the key; keep it. */
        c = ui_button(c, b2, _("o: open with"), HIT_KEY + 'o');
        /* TRANSLATORS: c is the key; keep it. */
        c = ui_button(c, b2, _("c: copy"), HIT_KEY + 'c');
        /* TRANSLATORS: x is the key; keep it. */
        c = ui_button(c, b2, _("x: cut"), HIT_KEY + 'x');
        /* TRANSLATORS: v is the key; keep it. */
        c = ui_button(c, b2, _("v: paste"), HIT_KEY + 'v');
        /* TRANSLATORS: r is the key; keep it. */
        c = ui_button(c, b2, _("r: rename"), HIT_KEY + 'r');
        /* TRANSLATORS: d is the key; keep it. */
        c = ui_button(c, b2, _("d: trash"), HIT_KEY + 'd');
        /* TRANSLATORS: n is the key; keep it. */
        c = ui_button(c, b2, _("n: new folder"), HIT_KEY + 'n');
        /* TRANSLATORS: u is the key; keep it. */
        c = ui_button(c, b2, _("u: undo"), HIT_KEY + 'u');
    }
    /* The way back is never the button left out: the others leave it room. */
    /* TRANSLATORS: q is the key; keep it. */
    const char *back = _("q: back");
    int keep = ui_cells(back) + 3;
    /* TRANSLATORS: / is the key; keep it. */
    c = button_before(1, b1, _("/: filter"), HIT_KEY + '/', keep);
    /* TRANSLATORS: . is the key; keep it. */
    c = button_before(c, b1, _(".: hidden"), HIT_KEY + '.', keep);
    /* TRANSLATORS: s is the key; keep it. */
    c = button_before(c, b1, _("s: sort"), HIT_KEY + 's', keep);
    /* TRANSLATORS: i is the key; keep it. */
    c = button_before(c, b1, _("i: props"), HIT_KEY + 'i', keep);
    /* TRANSLATORS: F3 is the key; keep it. */
    c = button_before(c, b1, _("F3: split"), HIT_KEY + K_F1 + 2, keep);
    /* TRANSLATORS: w is the key; keep it. */
    c = button_before(c, b1, _("w: wallpaper"), HIT_KEY + 'w', keep);
    /* TRANSLATORS: e is the key; keep it. */
    c = button_before(c, b1, _("e: eject"), HIT_KEY + 'e', keep);
    c = ui_button(c, b1, back, HIT_KEY + 'q');
    /* TRANSLATORS: Space, Tab and Ctrl+1/2/3 are keys; keep them. */
    if (c + 10 < cols) {
        screen_text(c, b1, _("Space: mark  Tab: panes  Ctrl+1/2/3: view"), FG, BG);
    }
    screen_flush();
}

/* Whole UTF-8 characters (no control characters) inserted at cur, replacing
 * the initial selection when all is set. */
static void insert_text(char *text, size_t size, size_t *cur, int *all, const char *s, size_t len)
{
    if (*all) {
        text[0] = '\0';
        *cur = 0;
        *all = 0;
    }
    char clean[512];
    size_t n = strlen(text), k = 0;
    for (size_t i = 0; i < len;) {
        unsigned char b = (unsigned char)s[i];
        size_t bytes = b < 128 ? 1 : b >= 0xC2 && b < 0xE0 ? 2 : b >= 0xE0 && b < 0xF0 ? 3 :
                       b >= 0xF0 && b <= 0xF4 ? 4 : 0;
        if (!bytes || i + bytes > len) break;
        int ok = b >= 32 && b != 127;
        for (size_t j = 1; j < bytes; j++) ok &= ((unsigned char)s[i + j] & 0xC0) == 0x80;
        if (ok && n + k + bytes < size && k + bytes <= sizeof clean) {
            memcpy(clean + k, s + i, bytes);
            k += bytes;
        }
        i += bytes;
    }
    memmove(text + *cur + k, text + *cur, n - *cur + 1);
    memcpy(text + *cur, clean, k);
    *cur += k;
}

/* A line typed at the bottom (UTF-8, Left/Right/Home/End/Delete/Backspace,
 * Ctrl+A, Ctrl+U, Ctrl+K, Ctrl+W, Ctrl+V from the clipboard); NULL if Esc. */
static const char *prompt(const char *question, const char *initial, int select_all)
{
    static char text[PATH_MAX];
    size_t cur = 0;
    int all = 0;
    text[0] = '\0';
    insert_text(text, sizeof text, &cur, &all, initial ? initial : "", initial ? strlen(initial) : 0);
    all = select_all && text[0] != '\0';
    for (;;) {
        int row = screen_rows() - 3, cols = screen_cols(), qcols = utf_cells(question, 0, strlen(question));
        int tcol = 2 + qcols, room = cols - tcol - 2;
        if (room < 4) room = 4;
        size_t left = 0;
        while (left < cur && utf_cells(text, left, cur) >= room) {
            left = utf_next(text, left);
        }
        char shown[PATH_MAX];
        snprintf(shown, sizeof shown, "%.*s", (int)utf_fit(text + left, room), text + left);
        screen_fill(0, row, cols, 1, BG);
        screen_text(1, row, question, ACCENT, BG);
        screen_text(tcol, row, shown, all ? BG : ACCENT, all ? ACCENT : BG);
        int ccol = tcol + utf_cells(text, left, cur);
        if (ccol < cols) {
            char ch[8] = "_";
            if (text[cur]) {
                size_t next = utf_next(text, cur);
                snprintf(ch, sizeof ch, "%.*s", (int)(next - cur), text + cur);
            }
            screen_text(ccol, row, ch, BG, ACCENT);
        }
        screen_flush();
        int key = key_poll_timeout(-1, -1), mods = keys_modifiers();
        size_t n = strlen(text);
        if (key == 27) {
            return NULL;
        } else if (key == '\n') {
            return text;
        } else if (key == 1 && (mods & KEYS_CTRL)) {
            cur = n;
            all = n > 0;
        } else if (key == 22 && (mods & KEYS_CTRL)) {
            int fd = -1;
            size_t len = 0;
            if (clipboard_get(0, &fd, &len) == CLIPBOARD_OK && fd >= 0) {
                char clip_text[1024];
                ssize_t got = read(fd, clip_text, len < sizeof clip_text ? len : sizeof clip_text);
                close(fd);
                if (got > 0) insert_text(text, sizeof text, &cur, &all, clip_text, (size_t)got);
            }
        } else if (key == 21 && (mods & KEYS_CTRL)) {
            memmove(text, text + cur, n - cur + 1);
            cur = all = 0;
        } else if (key == 11 && (mods & KEYS_CTRL)) {
            text[all ? 0 : cur] = '\0';
            if (all) cur = all = 0;
        } else if (key == 23 && (mods & KEYS_CTRL)) {
            if (all) {
                text[0] = '\0';
                cur = all = 0;
            } else {
                size_t p = cur;
                while (p > 0 && (text[p - 1] == ' ' || text[p - 1] == '/')) p--;
                while (p > 0 && text[p - 1] != ' ' && text[p - 1] != '/') p--;
                memmove(text + p, text + cur, n - cur + 1);
                cur = p;
            }
        } else if (key == K_LEFT || key == K_RIGHT || key == K_BOL || key == K_EOL) {
            if (all) {
                cur = key == K_LEFT || key == K_BOL ? 0 : n;
                all = 0;
            } else {
                cur = key == K_BOL ? 0 : key == K_EOL ? n :
                      key == K_LEFT ? utf_prev(text, cur) : utf_next(text, cur);
            }
        } else if (key == '\b') {
            if (all) {
                text[0] = '\0';
                cur = all = 0;
            } else if (cur > 0) {
                size_t p = utf_prev(text, cur);
                memmove(text + p, text + cur, n - cur + 1);
                cur = p;
            }
        } else if (key == K_DEL) {
            if (all) {
                text[0] = '\0';
                cur = all = 0;
            } else if (cur < n) {
                size_t next = utf_next(text, cur);
                memmove(text + cur, text + next, n - next + 1);
            }
        } else if (keys_typed(key) && !(mods & (KEYS_CTRL | KEYS_ALT))) {
            const char *typed = keys_typed(key);
            insert_text(text, sizeof text, &cur, &all, typed, strlen(typed));
        } else if (key == K_SHOWN) {
            draw();
        }
    }
}

static int yes(const char *question)
{
    char line[600];
    snprintf(line, sizeof line, "%s y/N ", question);
    int row = screen_rows() - 3;
    screen_fill(0, row, screen_cols(), 1, BG);
    screen_text(1, row, line, ERROR, BG);
    /* TRANSLATORS: Y is the key; keep it. */
    int c = ui_button(2 + ui_cells(line), row, _("Y: yes"), HIT_KEY + 'y');
    /* TRANSLATORS: N is the key; keep it. */
    ui_button(c, row, _("N: no"), HIT_KEY + 'n');
    screen_flush();
    for (;;) {
        int key = key_poll_timeout(-1, -1);
        if (key == K_POINTER) {
            const struct pointer_event *e = pointer_last();
            if (!e->pressed) continue; /* a motion is no answer */
            key = ui_hit_at(e->col, e->row) == HIT_KEY + 'y' ? 'y' : 'n';
        }
        return key == 'y' || key == 'Y';
    }
}

/* For properties: how many files and folders a folder holds, and their bytes. */
static long long tree_bytes;
static int tree_files, tree_dirs;

static int count_one(const char *path, const struct stat *st, int flag, struct FTW *ftw)
{
    (void)path;
    if (ftw->level == 0) return 0;
    if (flag == FTW_D || flag == FTW_DP) {
        tree_dirs++;
    } else {
        tree_files++;
        if (st && S_ISREG(st->st_mode)) tree_bytes += st->st_size;
    }
    return tree_files + tree_dirs > 200000 ? 1 : 0;
}

/* Alt+Return / i (or I for the current folder itself): PCManFM/Dolphin
 * Properties dialog with timestamps (mtime, atime, btime/ctime), disk usage,
 * filesystem free space, owner/group names, and interactive rwx chmod. */
static void properties(int folder_self)
{
    char path[PATH_MAX], target[PATH_MAX] = "", loc_dir[PATH_MAX] = "";
    struct file *f = NULL;
    const char *type = "inode/directory";
    const char *disp_name = "";
    int is_dir = 0;

    if (folder_self || P->nlist == 0) {
        if (is_trash(P)) return;
        const char *d = is_search(P) ? P->search_root : P->cwd;
        if (!d[0]) return;
        snprintf(path, sizeof path, "%s", d);
        parent_of(path, loc_dir, sizeof loc_dir);
        disp_name = base_name(path);
        is_dir = 1;
    } else {
        if (item_path(P, P->sel, path, sizeof path) < 0) return;
        f = &P->list[P->sel];
        type = item_type(P, P->sel);
        disp_name = shown_name(f);
        is_dir = f->dir;
        if (is_trash(P)) snprintf(loc_dir, sizeof loc_dir, "%s", TRASH_URL);
        else parent_of(path, loc_dir, sizeof loc_dir);
    }

    tree_bytes = tree_files = tree_dirs = 0;
    if (is_dir) {
        as_user(1);
        nftw(path, count_one, 16, FTW_PHYS);
        as_user(0);
    }
    for (;;) {
        struct stat st = { 0 }, lst = { 0 };
        struct statx stx;
        int has_btime = 0;
        struct statvfs vfs = { 0 };
        int has_vfs = 0;
        memset(&stx, 0, sizeof stx);
        as_user(1);
        lstat(path, &lst);
        if (stat(path, &st) < 0) st = lst;
        if (S_ISLNK(lst.st_mode)) {
            ssize_t n = readlink(path, target, sizeof target - 1);
            if (n >= 0) target[n] = '\0';
        }
        if (statx(AT_FDCWD, path, AT_SYMLINK_NOFOLLOW, STATX_BTIME, &stx) == 0 &&
            (stx.stx_mask & STATX_BTIME) && stx.stx_btime.tv_sec > 0) {
            has_btime = 1;
        }
        has_vfs = statvfs(path, &vfs) == 0 && vfs.f_blocks > 0;
        as_user(0);

        char size[192], ondisk[64], mdate[32], adate[32], cdate[32], ddate[32], perm[16], line[600], lbl[64];
        human((long long)st.st_blocks * 512LL, ondisk, sizeof ondisk);
        if (is_dir) {
            char b[32], files_str[48], dirs_str[48];
            human(tree_bytes, b, sizeof b);
            snprintf(files_str, sizeof files_str, P_("%d file", "%d files", tree_files), tree_files);
            snprintf(dirs_str, sizeof dirs_str, P_("%d folder", "%d folders", tree_dirs), tree_dirs);
            /* TRANSLATORS: first %s is human size (e.g. "12 KiB"), second %s is "%d file(s)", third %s is "%d folder(s)". */
            snprintf(size, sizeof size, _("%s (%s, %s)"), b, files_str, dirs_str);
        } else {
            human(st.st_size, size, sizeof size);
            snprintf(size + strlen(size), sizeof size - strlen(size),
                     _(" (%lld B, %s on disk)"), (long long)st.st_size, ondisk);
        }
        strftime(mdate, sizeof mdate, "%Y-%m-%d %H:%M:%S", localtime(&st.st_mtime));
        strftime(adate, sizeof adate, "%Y-%m-%d %H:%M:%S", localtime(&st.st_atime));
        time_t bsec = has_btime ? (time_t)stx.stx_btime.tv_sec : st.st_ctime;
        strftime(cdate, sizeof cdate, "%Y-%m-%d %H:%M:%S", localtime(&bsec));
        mode_string(st.st_mode, perm);

        int sw = side_width(), r0 = body_top();
        screen_fill(sw, r0, screen_cols() - sw, rows(), BG);
        ui_hits_clear();
        screen_text(sw + 1, r0, is_dir && (folder_self || !f) ? _("Folder Properties") : _("Properties"), ACCENT, BG);
        int r = r0 + 2;
        snprintf(line, sizeof line, "%s%.500s", ui_pad(lbl, sizeof lbl, _("Name:"), 14), disp_name);
        screen_text(sw + 1, r++, line, FG, BG);
        snprintf(line, sizeof line, "%s%.500s", ui_pad(lbl, sizeof lbl, _("Location:"), 14), loc_dir);
        screen_text(sw + 1, r++, line, FG, BG);
        if (f && f->origin && is_trash(P)) {
            snprintf(line, sizeof line, "%s%.500s", ui_pad(lbl, sizeof lbl, _("Original:"), 14), f->origin);
            screen_text(sw + 1, r++, line, FG, BG);
            strftime(ddate, sizeof ddate, "%Y-%m-%d %H:%M:%S", localtime(&f->deleted));
            snprintf(line, sizeof line, "%s%s", ui_pad(lbl, sizeof lbl, _("Deleted:"), 14), ddate);
            screen_text(sw + 1, r++, line, FG, BG);
        }
        if (target[0]) {
            snprintf(line, sizeof line, "%s%.500s", ui_pad(lbl, sizeof lbl, _("Points to:"), 14), target);
            screen_text(sw + 1, r++, line, FG, BG);
        }
        snprintf(line, sizeof line, "%s%s", ui_pad(lbl, sizeof lbl, _("Type:"), 14), type);
        screen_text(sw + 1, r++, line, FG, BG);
        snprintf(line, sizeof line, "%s%s", ui_pad(lbl, sizeof lbl, _("Size:"), 14), size);
        screen_text(sw + 1, r++, line, FG, BG);
        snprintf(line, sizeof line, "%s%s", ui_pad(lbl, sizeof lbl, _("Modified:"), 14), mdate);
        screen_text(sw + 1, r++, line, FG, BG);
        snprintf(line, sizeof line, "%s%s", ui_pad(lbl, sizeof lbl, _("Accessed:"), 14), adate);
        screen_text(sw + 1, r++, line, FG, BG);
        snprintf(line, sizeof line, "%s%s",
                 ui_pad(lbl, sizeof lbl, has_btime ? _("Created:") : _("Changed:"), 14), cdate);
        screen_text(sw + 1, r++, line, FG, BG);
        snprintf(line, sizeof line, "%s%s (%u) : %s (%u)",
                 ui_pad(lbl, sizeof lbl, _("Owner:"), 14),
                 uid_name(st.st_uid), (unsigned)st.st_uid, gid_name(st.st_gid), (unsigned)st.st_gid);
        screen_text(sw + 1, r++, line, FG, BG);
        snprintf(line, sizeof line, "%s%s (0%03o)",
                 ui_pad(lbl, sizeof lbl, _("Permissions:"), 14), perm, (unsigned)(st.st_mode & 07777));
        screen_text(sw + 1, r++, line, FG, BG);
        if (has_vfs) {
            char ffree[32], ftotal[32];
            human((long long)vfs.f_bavail * (long long)vfs.f_frsize, ffree, sizeof ffree);
            human((long long)vfs.f_blocks * (long long)vfs.f_frsize, ftotal, sizeof ftotal);
            snprintf(line, sizeof line, _("%s%s free of %s"),
                     ui_pad(lbl, sizeof lbl, _("Filesystem:"), 14), ffree, ftotal);
            screen_text(sw + 1, r++, line, FG, BG);
        }

        int c = sw + 1;
        if (!pick_type && !is_trash(P)) {
            if (S_ISREG(st.st_mode)) {
                /* TRANSLATORS: x is the key; keep it. */
                c = ui_button(c, r + 1, (st.st_mode & 0111) ? _("x: -exec") : _("x: +exec"), HIT_KEY + 'x');
            }
            if (!S_ISLNK(lst.st_mode)) {
                /* TRANSLATORS: m is the key; keep it. */
                c = ui_button(c, r + 1, _("m: chmod..."), HIT_KEY + 'm');
            }
        }
        /* TRANSLATORS: Enter and Esc are keys; keep them. */
        ui_button(c, r + 1, _("Enter / Esc: close"), HIT_KEY + 27);
        screen_flush();

        int key = key_poll_timeout(-1, -1);
        if (key == K_POINTER) {
            const struct pointer_event *e = pointer_last();
            int id = e->pressed && e->button == BTN_LEFT ? ui_hit_at(e->col, e->row) : -1;
            key = id >= HIT_KEY ? (id - HIT_KEY) & 0xFFFF : 0;
        }
        if (key == 'x' && !pick_type && !is_trash(P) && S_ISREG(st.st_mode)) {
            mode_t next = (st.st_mode & 0111) ? (st.st_mode & ~0111) : (st.st_mode | 0111);
            as_user(1);
            int ok = chmod(path, next & 07777) == 0;
            as_user(0);
            if (ok && f) {
                f->exec = (next & 0111) != 0;
                f->mode = (f->mode & ~07777) | (next & 07777);
            }
        } else if (key == 'm' && !pick_type && !is_trash(P) && !S_ISLNK(lst.st_mode)) {
            char cur_oct[16];
            snprintf(cur_oct, sizeof cur_oct, "%03o", (unsigned)(st.st_mode & 07777));
            const char *in = prompt(_("Octal permissions (e.g. 644 or 755):"), cur_oct, 1);
            if (in && in[0]) {
                char *end = NULL;
                unsigned long val = strtoul(in, &end, 8);
                if (end && *end == '\0' && val <= 07777UL) {
                    as_user(1);
                    int ok = chmod(path, (mode_t)val) == 0;
                    as_user(0);
                    if (ok && f) {
                        f->exec = S_ISREG(st.st_mode) && (val & 0111) != 0;
                        f->mode = (f->mode & ~07777) | (mode_t)val;
                    }
                }
            }
        } else if (key == 27 || key == 'q' || key == '\n' || key == 'i' || key == 'I') {
            return;
        }
    }
}

/* mountinfo writes a space as \040 (and \011 tab, \012 newline, \134 \). */
static void unescape(char *s)
{
    char *out = s;
    for (; *s; s++) {
        if (s[0] == '\\' && s[1] >= '0' && s[1] <= '3' && s[2] >= '0' && s[2] <= '7' &&
            s[3] >= '0' && s[3] <= '7') {
            *out++ = (char)((s[1] - '0') * 64 + (s[2] - '0') * 8 + (s[3] - '0'));
            s += 3;
        } else {
            *out++ = *s;
        }
    }
    *out = '\0';
}

/* A drive in ~/Drives: unmounted, then its USB device powered off, so that
 * it can be pulled out. Not while something has files open on it, here or
 * in a container (then the device is still claimed). */
static void eject(const char *path, const char *name)
{
    char line[2048], dev[128] = "";
    FILE *in = fopen("/proc/self/mountinfo", "re");
    while (in && fgets(line, sizeof line, in)) {
        char mp[PATH_MAX], src[128];
        const char *dash = strstr(line, " - ");
        if (sscanf(line, "%*s %*s %*s %*s %4095s", mp) != 1 || !dash) {
            continue;
        }
        unescape(mp);
        if (strcmp(mp, path) == 0 && sscanf(dash + 3, "%*s %127s", src) == 1) {
            snprintf(dev, sizeof dev, "%s", src);
        }
    }
    if (in) {
        fclose(in);
    }
    if (strncmp(dev, "/dev/", 5) != 0) {
        /* TRANSLATORS: Drives is the folder name ~/Drives; keep it. */
        report(1, _("%s is not a drive (only those in Drives are ejected)."), name);
        return;
    }
    sync();
    if (umount2(path, 0) < 0) {
        report(1, _("%s is in use: close the files open on it first."), name);
        return;
    }
    int fd = open(dev, O_RDONLY | O_EXCL | O_CLOEXEC);
    if (fd < 0) {
        report(1, _("%s is still in use in a container: stop it before pulling the drive out."), name);
        return;
    }
    close(fd);
    /* The disk of the partition: /sys/class/block/sdb1 is .../sdb/sdb1. */
    char sys[PATH_MAX], disk[PATH_MAX], file[PATH_MAX + 32];
    struct stat st;
    snprintf(sys, sizeof sys, "/sys/class/block/%s", dev + 5);
    snprintf(file, sizeof file, "%s/partition", sys);
    if (realpath(sys, disk) == NULL) {
        disk[0] = '\0';
    } else if (stat(file, &st) == 0) {
        *strrchr(disk, '/') = '\0';
    }
    snprintf(file, sizeof file, "%s/device/delete", disk);
    FILE *off = disk[0] ? fopen(file, "we") : NULL;
    int powered_off = off && fputs("1", off) >= 0;
    if (off && fclose(off) != 0) {
        powered_off = 0;
    }
    say("files: ejected %s (%s)%s", path, dev, powered_off ? ", powered off" : "");
    rmdir(path);
    report(0, _("%s can be pulled out now."), name);
    reload_all();
}

/* The chooser: the containers that can open type (or all of them). */
static int choose(const char *path, const char *type, char *name, size_t size)
{
    char names[24][OPEN_NAME], rule[OPEN_NAME] = "";
    int all = 0, n = open_candidates(type, names, 24), at = 0, r0 = body_top();
    if (n == 0) {
        all = 1;
        n = open_containers(names, 24);
    }
    if (n == 0) {
        report(1, _("There is no container to open it with."));
        return 0;
    }
    int is_rule = open_rule(type, rule, sizeof rule) == OPEN_RULE_CONTAINER;
    for (int i = 0; is_rule && i < n; i++) {
        if (strcmp(names[i], rule) == 0) {
            at = i;
        }
    }
    for (;;) {
        char line[512], label[160], padded[256];
        int last = n + !all - 1;
        const char *slash = path[0] == '/' ? strrchr(path, '/') : NULL;
        screen_fill(SIDE, r0, screen_cols() - SIDE, rows(), BG);
        /* TRANSLATORS: first %s is a file name or URL, second %s is its MIME type. */
        snprintf(line, sizeof line, _("Open %s (%s) with:"), slash ? slash + 1 : path, type);
        screen_text(SIDE + 1, r0, line, ACCENT, BG);
        for (int i = 0; i <= last; i++) {
            if (i < n) {
                open_label(names[i], type, label, sizeof label);
                pad(padded, sizeof padded, label, 50);
                snprintf(line, sizeof line, "%2d  %s %s", i + 1, padded,
                         is_rule && strcmp(names[i], rule) == 0 ? _("default") : "");
            } else {
                snprintf(line, sizeof line, "%2d  %s", i + 1, _("Other container..."));
            }
            screen_fill(SIDE, r0 + 2 + i, screen_cols() - SIDE, 1, i == at ? ACCENT : BG);
            screen_text(SIDE + 1, r0 + 2 + i, line, i == at ? BG : FG, i == at ? ACCENT : BG);
        }
        ui_hits_clear();
        for (int i = 0; i <= last; i++) ui_hit(SIDE, r0 + 2 + i, screen_cols() - SIDE, 1, HIT_FILE + i);
        /* TRANSLATORS: Enter is the key; keep it. */
        int c = ui_button(SIDE + 1, r0 + 4 + last, _("Enter: open"), HIT_KEY + '\n');
        /* TRANSLATORS: A is the key; keep it. */
        c = ui_button(c, r0 + 4 + last, _("A: always use it"), HIT_KEY + 'a');
        /* TRANSLATORS: Esc is the key; keep it. */
        ui_button(c, r0 + 4 + last, _("Esc: cancel"), HIT_KEY + 27);
        screen_flush();
        int key = key_poll_timeout(-1, -1);
        if (key == K_POINTER) {
            const struct pointer_event *e = pointer_last();
            int id = e->pressed && e->button == BTN_LEFT ? ui_hit_at(e->col, e->row) : -1;
            if (id >= HIT_KEY) {
                key = id - HIT_KEY;
            } else if (id >= HIT_FILE && id - HIT_FILE <= last) {
                at = id - HIT_FILE;
                key = e->clicks == 2 ? '\n' : 0;
            } else if (e->wheel) {
                at = at - e->wheel < 0 ? 0 : at - e->wheel > last ? last : at - e->wheel;
            }
        }
        if (key == 27 || key == 'q') {
            report(0, _("Not opened."));
            return 0;
        } else if (key == K_UP && at > 0) {
            at--;
        } else if (key == K_DOWN && at < last) {
            at++;
        } else if (key >= '1' && key <= '9' && key - '1' <= last) {
            at = key - '1';
        } else if (key == '\n' || key == 'a' || key == 'A') {
            if (at == n) {
                all = 1;
                n = open_containers(names, 24);
                at = 0;
                continue;
            }
            if (key != '\n') {
                open_set_rule(type, names[at]);
            }
            snprintf(name, size, "%s", names[at]);
            return 1;
        }
    }
}

/* Open path (or a URL) by the rule for its type or the user's choice (pick:
 * always asked), or with the framebuffer program with. */
static void open_file(const char *path, int pick, const char *with)
{
    char type[128], name[OPEN_NAME] = "", why[512], names[2][OPEN_NAME], spec[256], *argv[16];
    as_user(1);
    int type_ok = open_type(path, type, sizeof type) == 0;
    as_user(0);
    if (!type_ok) {
        report(1, _("Cannot tell what %s is."), path);
        return;
    }
    int kind = open_rule(type, name, sizeof name);
    if (with) {
        snprintf(name, sizeof name, "%s", with);
    } else if (pick || kind == OPEN_RULE_ASK || (kind == OPEN_NO_RULE && open_candidates(type, names, 2) != 1)) {
        if (!choose(path, type, name, sizeof name)) {
            return;
        }
    } else if (kind == OPEN_NO_RULE) {
        snprintf(name, sizeof name, "%s", names[0]);
    }
    /* A framebuffer program: a task of its own, on this screen; it sees the
     * user's files as the user. */
    if (open_program(name, path, spec, sizeof spec, argv, 16)) {
        say("home: open %s (%s) with %s", path, type, name);
        base_open(argv, why, sizeof why);
        say("home: %s", why);
        report(0, _("Opened with %s."), name + strlen(OPEN_PROGRAM));
        return;
    }
    if (with) {
        report(1, _("There is no program %s."), with);
        return;
    }
    /* A container sees the shared folders only; a host program, the home. */
    if (path[0] == '/' && !open_host(name) && !open_shared(path, why, sizeof why)) {
        report(1, _("%.400s: containers cannot see it."), why);
        return;
    }
    say("home: open %s (%s) in %s", path, type, name);
    if (base_app_open(name, path, why, sizeof why) == 0) {
        report(0, _("Opened in %s."), name);
    } else {
        report(1, "%s: %.400s", name, why);
    }
}

/* What a program on the screen or a container asked home to open
 * (open_in_home): its folder shown, it selected, and opened. */
static int take_open_request(void)
{
    char with[512] = "", target[PATH_MAX + 16] = "", path[PATH_MAX];
    FILE *in = fopen(OPEN_REQUEST, "re");
    if (!in) {
        return 0;
    }
    int got = fgets(with, sizeof with, in) && fgets(target, sizeof target, in);
    fclose(in);
    unlink(OPEN_REQUEST);
    with[strcspn(with, "\n")] = '\0';
    target[strcspn(target, "\n")] = '\0';
    if (!got || strlen(with) >= OPEN_NAME || open_normalize(target, path, sizeof path) < 0) {
        return 0;
    }
    char *slash = path[0] == '/' ? strrchr(path, '/') : NULL;
    if (slash && slash != path) {
        char dir[PATH_MAX];
        snprintf(dir, sizeof dir, "%.*s", (int)(slash - path), path);
        go(dir, slash + 1);
    }
    /* "ask": the user chooses (archive's Open With...). */
    if (strcmp(with, "ask") == 0) open_file(path, 1, NULL);
    else open_file(path, 0, with[0] ? with : NULL);
    return 1;
}

/* A folder (or a file in it) a container asked to show (FILES_REQUEST): inside
 * the user's home, a folder's newest file selected, or the file itself. */
static int take_folder_request(void)
{
    char raw[PATH_MAX + 16], path[PATH_MAX], real[PATH_MAX], dir[PATH_MAX];
    struct stat st;
    FILE *in = fopen(FILES_REQUEST, "re");
    if (!in) {
        return 0;
    }
    int got = fgets(raw, sizeof raw, in) != NULL;
    fclose(in);
    unlink(FILES_REQUEST);
    raw[got ? strcspn(raw, "\n") : 0] = '\0';
    if (!got || open_normalize(raw, path, sizeof path) < 0) {
        return 0;
    }
    as_user(1);
    int ok = realpath(path, real) != NULL && stat(real, &st) == 0;
    as_user(0);
    if (!ok || !inside(real, home)) {
        report(1, _("Only the home (%s) is shown for a container."), home);
        return 0;
    }
    if (S_ISDIR(st.st_mode)) {
        if (go(real, NULL) < 0) return 0;
        for (int i = 0; i < P->nlist; i++) {
            if (!P->list[i].dir && (P->list[P->sel].dir || P->list[i].mtime > P->list[P->sel].mtime)) {
                P->sel = i;
            }
        }
    } else {
        parent_of(real, dir, sizeof dir);
        if (go(dir, base_name(real)) < 0) return 0;
    }
    /* TRANSLATORS: Enter and o are keys; keep them. */
    report(0, _("Shown for a container: Enter or o opens the selected file."));
    return 1;
}

static int take_request(void)
{
    int folder = take_folder_request();
    return take_open_request() || folder;
}

int files_request(void)
{
    return access(FILES_REQUEST, F_OK) == 0 || access(OPEN_REQUEST, F_OK) == 0;
}

/* The user's home, its shared folders and the Trash, as places; the first
 * tab when there is none yet. -1 if there is no user. */
static int setup(void)
{
    open_user_dirs(home, sizeof home, dirs, sizeof dirs);
    if (!home[0]) {
        return -1;
    }
    if (ntabs == 0) {
        ntabs = 1;
        cur_tab = 0;
        tabs[0].pane[0].view = VIEW_DETAILS;
        memcpy(tabs[0].pane[0].zoom, zoom_default, sizeof zoom_default);
        load_prefs();
    }
    rebuild_places();
    side = side_sel = 0;
    set_active();
    msg[0] = '\0';
    msg_sticky = 0;
    return 0;
}

static void apply_sort_choice(int col_sort)
{
    char keep[256];
    snprintf(keep, sizeof keep, "%s", selected_name(P));
    if (P->sort == col_sort) {
        P->desc = !P->desc;
    } else {
        P->sort = col_sort;
        P->desc = 0;
    }
    sort_pane(P, keep);
    save_prefs();
}

static void sort_menu(int col, int row)
{
    static const int skeys[] = {
        SORT_NAME, SORT_MTIME, SORT_SIZE, SORT_TYPE, SORT_EXT, SORT_PERM, SORT_OWNER
    };
    static const char *const slabels[] = {
        N_("By Name (Ctrl+Alt+1)"),
        N_("By Modification Time (Ctrl+Alt+2)"),
        N_("By Size (Ctrl+Alt+3)"),
        N_("By File Type (Ctrl+Alt+4)"),
        N_("By Extension (Ctrl+Alt+5)"),
        N_("By Permissions"),
        N_("By Owner"),
    };
    char rows_buf[10][96];
    const char *items[11];
    int sk = sort_key(P);
    for (int i = 0; i < 7; i++) {
        int active = sk == skeys[i] || (i == 1 && sk == SORT_DELETED) || (i == 3 && sk == SORT_ORIGIN);
        snprintf(rows_buf[i], sizeof rows_buf[i], "%s %s",
                 active ? (P->desc ? "[v]" : "[^]") : "[ ]", _(slabels[i]));
        items[i] = rows_buf[i];
    }
    snprintf(rows_buf[7], sizeof rows_buf[7], "[%c] %s", P->desc ? 'x' : ' ', _("Descending order"));
    snprintf(rows_buf[8], sizeof rows_buf[8], "[%c] %s", mingle_dirs ? 'x' : ' ', _("Mingle files and folders"));
    snprintf(rows_buf[9], sizeof rows_buf[9], "[%c] %s", sort_case ? 'x' : ' ', _("Case-sensitive name sort"));
    items[7] = rows_buf[7];
    items[8] = rows_buf[8];
    items[9] = rows_buf[9];
    items[10] = NULL;

    int choice = ui_menu(col, row, items);
    if (choice < 0) return;
    char keep[256];
    snprintf(keep, sizeof keep, "%s", selected_name(P));
    if (choice < 7) {
        apply_sort_choice(skeys[choice]);
        return;
    } else if (choice == 7) {
        P->desc = !P->desc;
    } else if (choice == 8) {
        mingle_dirs = !mingle_dirs;
    } else if (choice == 9) {
        sort_case = !sort_case;
    }
    sort_pane(P, keep);
    save_prefs();
}

static void columns_menu(int col, int row)
{
    static const int flags[] = { COL_SIZE, COL_MTIME, COL_EXT, COL_PERM, COL_OWNER, COL_TYPE };
    static const char *const names[] = {
        N_("Size / folder item count"),
        N_("Modified time"),
        N_("Extension"),
        N_("Permissions (rwx)"),
        N_("Owner (user:group)"),
        N_("Type / Location"),
    };
    char rows_buf[6][80];
    const char *items[7];
    for (int i = 0; i < 6; i++) {
        snprintf(rows_buf[i], sizeof rows_buf[i], "[%c] %s", (cols_mask & flags[i]) ? 'x' : ' ', _(names[i]));
        items[i] = rows_buf[i];
    }
    items[6] = NULL;
    int choice = ui_menu(col, row, items);
    if (choice >= 0 && choice < 6) {
        cols_mask ^= flags[choice];
        save_prefs();
    }
}

/* Ark's Extract and Compress menus (Dolphin's, from Ark's plugins): done by
 * archive, through its command line as Ark's are through Ark's. What the
 * menus give for them, not keys. */
#ifndef ARCHIVE_PROGRAM
#define ARCHIVE_PROGRAM "/usr/local/bin/archive"
#endif
enum {
    KEY_EXTRACT_HERE = 0x1E0, KEY_EXTRACT_TRASH, KEY_EXTRACT_TO, KEY_COMPRESS_TGZ, KEY_COMPRESS_ZIP,
    KEY_COMPRESS_TO, KEY_EXTRACT_MENU, KEY_COMPRESS_MENU
};

/* Item i is an archive archive opens (the types of its .desktop). */
static int is_archive(struct pane *p, int i)
{
    char names[16][OPEN_NAME];
    int n = open_candidates(item_type(p, i), names, 16);
    for (int k = 0; k < n; k++) {
        if (strcmp(names[k], OPEN_PROGRAM "archive") == 0) return 1;
    }
    return 0;
}

/* The user can write in dir (Ark offers "here" only then). */
static int folder_writable(const char *dir)
{
    struct stat st;
    as_user(1);
    int ok = stat(dir, &st) == 0;
    as_user(0);
    return ok && (st.st_uid == USER ? (st.st_mode & S_IWUSR) != 0 :
                  st.st_gid == USER ? (st.st_mode & S_IWGRP) != 0 : (st.st_mode & S_IWOTH) != 0);
}

/* What the targets are compressed into here with suffix, as Ark names it
 * (their common name, "Archive" for several; "NAME (1)" if there is one). */
static void compress_name(const int *idx, int n, const char *suffix, char *out, size_t size)
{
    const char *names[MAX_CLIP];
    char base[NAME_MAX + 1], want[NAME_MAX + 16];
    for (int i = 0; i < n; i++) names[i] = P->list[idx[i]].name;
    archive_name_for(names, n, _("Archive"), base, sizeof base);
    snprintf(want, sizeof want, "%s%s", base, suffix);
    as_user(1);
    if (archive_suggest_name(P->cwd, want, out, size) < 0) snprintf(out, size, "%s", want);
    as_user(0);
}

static int archive_menu(int col, int row, int extract)
{
    const char *items[4];
    int codes[4], n = 0, idx[MAX_CLIP], nt = targets(P, idx, MAX_CLIP), here = folder_writable(P->cwd);
    char tgz[NAME_MAX + 48], zip[NAME_MAX + 48], name[NAME_MAX + 16];
#define ITEM(label, code) (items[n] = (label), codes[n++] = (code))
    if (extract && here) {
        ITEM(_("Extract here"), KEY_EXTRACT_HERE);
        ITEM(_("Extract and trash archive"), KEY_EXTRACT_TRASH);
    }
    if (extract) ITEM(_("Extract to..."), KEY_EXTRACT_TO);
    if (!extract && here) {
        compress_name(idx, nt, ".tar.gz", name, sizeof name);
        /* TRANSLATORS: %s is the archive's file name. */
        snprintf(tgz, sizeof tgz, _("Compress to \"%s\""), name);
        ITEM(tgz, KEY_COMPRESS_TGZ);
        compress_name(idx, nt, ".zip", name, sizeof name);
        snprintf(zip, sizeof zip, _("Compress to \"%s\""), name);
        ITEM(zip, KEY_COMPRESS_ZIP);
    }
    if (!extract) ITEM(_("Compress to..."), KEY_COMPRESS_TO);
#undef ITEM
    items[n] = NULL;
    int got = ui_menu(col, row, items);
    return got < 0 ? -1 : codes[got];
}

/* archive, as the menus' code says, on the targets; then this folder again,
 * a new archive selected in it. Extract and trash: the archives go to the
 * Trash when all came out (Dolphin's plugin does it in Ark). */
static void archive_action(int code)
{
    int idx[MAX_CLIP], n = targets(P, idx, MAX_CLIP), k = 0, status;
    char *argv[MAX_CLIP + 8], name[NAME_MAX + 16] = "", made[PATH_MAX];
    if (n == 0 || is_trash(P) || is_search(P)) return;
    argv[k++] = ARCHIVE_PROGRAM;
    if (code == KEY_EXTRACT_HERE || code == KEY_EXTRACT_TRASH) {
        argv[k++] = "--batch";
        argv[k++] = "--autodestination";
        argv[k++] = "--autosubfolder";
    } else if (code == KEY_EXTRACT_TO) {
        argv[k++] = "--batch";
        argv[k++] = "--dialog";
    } else if (code == KEY_COMPRESS_TGZ || code == KEY_COMPRESS_ZIP) {
        compress_name(idx, n, code == KEY_COMPRESS_TGZ ? ".tar.gz" : ".zip", name, sizeof name);
        if (join(made, sizeof made, P->cwd, name) != 0) return;
        argv[k++] = "--add-to";
        argv[k++] = made;
        argv[k++] = "--changetofirstpath";
    } else {
        argv[k++] = "--add";
        argv[k++] = "--changetofirstpath";
    }
    int first = k;
    for (int i = 0; i < n; i++) {
        char path[PATH_MAX];
        if (item_path(P, idx[i], path, sizeof path) == 0 && (argv[k] = strdup(path)) != NULL) k++;
    }
    argv[k] = NULL;
    status = base_call(argv);
    for (int i = first; i < k; i++) free(argv[i]);
    if (code == KEY_EXTRACT_TRASH && status == 0) {
        trash_items(P, idx, n);
        return;
    }
    reload_all();
    for (int i = 0; name[0] && i < P->nlist; i++) {
        if (strcmp(P->list[i].name, name) == 0) P->sel = i;
    }
    if (status == 0) {
        report(0, "%s", code <= KEY_EXTRACT_TO ? _("Extracted.") : name[0] ? name : _("Compressed."));
    } else if (status == 2) {
        report(1, "%s", _("Cancelled."));
    } else {
        /* TRANSLATORS: %d is the program's exit status. */
        report(1, _("archive did not finish (status %d)."), status);
    }
}

/* The menu of the selected item (or of the folder, on no item), as keys (+ MODS). */
static int file_menu(int col, int row, int on_file)
{
    const char *items[24];
    int keys[24], n = 0, trash = is_trash(P), search = is_search(P);
#define ITEM(label, key) (items[n] = (label), keys[n++] = (key))
    if (on_file && pick_type) {
        ITEM(P->list[P->sel].dir ? _("Go in") : _("Choose"), '\n');
        if (picking_folder() && P->list[P->sel].dir) ITEM(_("Choose this folder"), 'o');
    } else if (on_file && trash) {
        ITEM(_("Restore"), 'r');
        ITEM(_("Delete permanently..."), 'd');
        ITEM(_("Empty Trash..."), 'e');
        ITEM(_("Properties"), 'i');
    } else if (on_file) {
        ITEM(_("Open"), '\n');
        if (!P->list[P->sel].dir) ITEM(_("Open with..."), 'o');
        ITEM(_("Copy"), 'c');
        ITEM(_("Cut"), 'x');
        if (nclip && !search) ITEM(_("Paste"), 'v');
        ITEM(_("Rename..."), 'r');
        ITEM(_("Move to Trash"), 'd');
        ITEM(_("Delete permanently..."), 'D');
        char path[PATH_MAX], other[PATH_MAX];
        if (!P->list[P->sel].dir && strncmp(item_type(P, P->sel), "image/", 6) == 0) {
            ITEM(_("Set as wallpaper"), 'w');
        }
        if (P->list[P->sel].dir && join(other, sizeof other, home, "Drives") == 0 && strcmp(P->cwd, other) == 0 &&
            join(path, sizeof path, P->cwd, P->list[P->sel].name) == 0) {
            ITEM(_("Eject"), 'e');
        }
        if (!search) {
            ITEM(_("Duplicate (Ctrl+U)"), MODS(KEYS_CTRL) + ('u' & 0x1F));
            ITEM(_("Create link... (L)"), 'L');
        }
        ITEM(_("Copy path(s) (Ctrl+Shift+C)"), MODS(KEYS_CTRL | KEYS_SHIFT) + ('c' & 0x1F));
        /* Ark's, where Dolphin has its service menus: above Properties. */
        if (!search && access(ARCHIVE_PROGRAM, X_OK) == 0) {
            int idx[MAX_CLIP], nt = targets(P, idx, MAX_CLIP), archives = 0;
            for (int k = 0; k < nt; k++) archives += !P->list[idx[k]].dir && is_archive(P, idx[k]);
            if (nt && archives == nt) ITEM(_("Extract"), KEY_EXTRACT_MENU);
            /* Not for one archive alone (Ark's #268163). */
            if (!(nt == 1 && archives == 1)) ITEM(_("Compress"), KEY_COMPRESS_MENU);
        }
        ITEM(_("Properties"), 'i');
    } else if (!pick_type && trash) {
        ITEM(_("Empty Trash..."), 'e');
    } else if (!pick_type && !search) {
        ITEM(_("New folder..."), 'n');
        if (nclip) ITEM(_("Paste"), 'v');
        ITEM(_("New empty file... (N)"), 'N');
        ITEM(_("Bookmark folder (Ctrl+D)"), MODS(KEYS_CTRL) + ('d' & 0x1F));
        ITEM(_("Open in terminal (F4)"), K_F1 + 3);
        ITEM(_("Find files... (Ctrl+Shift+F)"), MODS(KEYS_CTRL | KEYS_SHIFT) + ('f' & 0x1F));
        ITEM(_("Folder properties (I)"), 'I');
    }
    if (!pick_type && nundo) {
        ITEM(_("Undo"), 'u');
    }
    ITEM(_("Sort files... (s)"), 's');
    if (P->view == VIEW_DETAILS) {
        ITEM(_("Columns... (C)"), 'C');
    }
    ITEM(hidden ? _("Hide hidden files") : _("Show hidden files"), '.');
#undef ITEM
    items[n] = NULL;
    int choice = ui_menu(col + 1, row, items);
    if (choice >= 0 && (keys[choice] == KEY_EXTRACT_MENU || keys[choice] == KEY_COMPRESS_MENU)) {
        return archive_menu(col + 4, row + choice, keys[choice] == KEY_EXTRACT_MENU);
    }
    return choice < 0 ? -1 : keys[choice];
}

static void zoom_pane(struct pane *p, int step)
{
    int v = p->view;
    p->zoom[v] = step == 0 ? zoom_default[v] : clampi(p->zoom[v] + step, 0, zoom_max[v]);
    static const int px[] = { 16, 22, 32, 48, 64, 96, 128 };
    if (v == VIEW_ICONS) {
        report(0, _("Icons: %d px."), px[p->zoom[v]]);
    } else if (v == VIEW_COMPACT) {
        report(0, _("Compact zoom: %d."), p->zoom[v] + 1);
    } else {
        report(0, _("Details zoom: %d."), p->zoom[v] + 1);
    }
}

static void drag_stop(void)
{
    free(drag.base);
    memset(&drag, 0, sizeof drag);
}

/* The marks of drag.pane's items updated from the rubberband rectangle in
 * document pixels: Shift adds to the marks at the press, Ctrl toggles them,
 * a plain drag marks what the rectangle touches. */
static void band_mark(void)
{
    struct pane *p = &tabs[cur_tab].pane[drag.pane];
    int col, row, cols, nrows, gc, tc, tr, vr;
    pane_area(p, drag.pane, &col, &row, &cols, &nrows);
    pane_grid(p, cols, nrows, &gc, &tc, &tr, &vr);
    int ox = col * cell_w, oy = top_px + row * cell_h, scroll = (p->top / gc) * tr * cell_h;
    int bx = drag.x - ox, by = drag.y - oy + scroll;
    int x0 = drag.ax < bx ? drag.ax : bx, x1 = drag.ax > bx ? drag.ax : bx;
    int y0 = drag.ay < by ? drag.ay : by, y1 = drag.ay > by ? drag.ay : by;
    int tw = (p->view == VIEW_DETAILS ? cols : tc) * cell_w, th = tr * cell_h;
    int first = -1, last = -1;
    for (int i = 0; i < p->nlist; i++) {
        int ix0 = (i % gc) * tc * cell_w, iy0 = (i / gc) * th;
        int hit = ix0 < x1 + 1 && ix0 + tw > x0 && iy0 < y1 + 1 && iy0 + th > y0;
        int was = i < drag.nbase ? drag.base[i] : 0;
        p->list[i].marked = (drag.mods & KEYS_CTRL) ? (hit ? !was : was) :
                            (drag.mods & KEYS_SHIFT) ? (was || hit) : hit;
        if (hit && first < 0) first = i;
        if (hit) last = i;
    }
    if (last >= 0) p->sel = by >= drag.ay ? last : first;
}

/* One auto-scroll step while the rubberband is held at or past the top or
 * bottom of the pane's viewport. 1 if it scrolled. */
static int band_autoscroll(void)
{
    struct pane *p = &tabs[cur_tab].pane[drag.pane];
    int col, row, cols, nrows, gc, tc, tr, vr;
    pane_area(p, drag.pane, &col, &row, &cols, &nrows);
    pane_grid(p, cols, nrows, &gc, &tc, &tr, &vr);
    int tr0 = p->top / gc, max_row = p->nlist > 0 ? (p->nlist - 1) / gc + 1 - vr : 0;
    if (max_row < 0) max_row = 0;
    int next = drag.row <= row + 1 ? tr0 - 1 : drag.row >= row + nrows - 2 ? tr0 + 1 : tr0;
    next = clampi(next, 0, max_row);
    if (next == tr0) return 0;
    p->top = next * gc;
    band_mark();
    return 1;
}

static void pointer_xy(const struct pointer_event *e, int *x, int *y)
{
    cell_metrics();
    *x = e->x || e->y ? e->x : e->col * cell_w + cell_w / 2;
    *y = e->x || e->y ? e->y : top_px + e->row * cell_h + cell_h / 2;
}

static void place_go(int i)
{
    if (strcmp(places[i], TRASH_URL) != 0) {
        as_user(1);
        mkdir(places[i], 0755);
        as_user(0);
    }
    go(places[i], NULL);
}

/* What a pointer event does: the key (+ MODS) it stands for, -1 to redraw
 * only, 0 for nothing. */
static int pointer_key(int *out_mods)
{
    const struct pointer_event *e = pointer_last();
    int mods = keys_modifiers(), px, py;
    pointer_xy(e, &px, &py);
    if (e->gesture == GESTURE_PINCH) {
        if (e->scale >= 1.15 || e->scale <= 0.87) {
            zoom_pane(P, e->scale > 1.0 ? 1 : -1);
            return -1;
        }
        return 0;
    }
    if (e->wheel) {
        if (e->col < SIDE) {
            side_sel = clampi(side_sel - e->wheel, 0, nplaces - 1);
            return -1;
        }
        int which = 0;
        if (tabs[cur_tab].split) {
            int c0, w0;
            pane_cols(0, &c0, &w0);
            which = e->col > c0 + w0;
        }
        tabs[cur_tab].active = which;
        set_active();
        if (mods & KEYS_CTRL) {
            zoom_pane(P, e->wheel > 0 ? 1 : -1);
            return -1;
        }
        int col, row, cols, nrows, gc, tc, tr, vr;
        pane_area(P, which, &col, &row, &cols, &nrows);
        pane_grid(P, cols, nrows, &gc, &tc, &tr, &vr);
        int step = 3 * e->wheel * gc;
        P->sel = clampi(P->sel - step, 0, P->nlist > 0 ? P->nlist - 1 : 0);
        P->top = clampi(P->top - step, 0, P->nlist > 0 ? P->nlist - 1 : 0);
        return -1;
    }

    /* A drag in progress: a motion extends the rubberband (or starts a drag
     * of items once the pointer moves to another cell); the release drops. */
    if (!e->button && drag.mode != DRAG_NONE) {
        drag.x = px;
        drag.y = py;
        drag.col = e->col;
        drag.row = e->row;
        drag.mods = mods;
        if (drag.mode == DRAG_PENDING && (e->col != drag.ax || e->row != drag.ay)) {
            drag.mode = DRAG_ITEMS;
            return -1;
        }
        if (drag.mode == DRAG_BAND) {
            band_mark();
            band_autoscroll();
            return -1;
        }
        return drag.mode == DRAG_ITEMS ? -1 : 0;
    }
    if (e->button == BTN_LEFT && !e->pressed && drag.mode != DRAG_NONE) {
        int mode = drag.mode, from_pane = drag.pane, item = drag.item, clear = drag.clear;
        struct pane *fp = &tabs[cur_tab].pane[from_pane];
        drag_stop();
        if (mode == DRAG_PENDING && clear) {
            for (int k = 0; k < fp->nlist; k++) fp->list[k].marked = 0;
            return -1;
        }
        if (mode == DRAG_ITEMS && !pick_type) {
            int id = ui_hit_at(e->col, e->row), idx[MAX_CLIP], n = 0;
            if (item >= 0 && item < fp->nlist && !fp->list[item].marked) {
                idx[n++] = item;
            } else {
                n = targets(fp, idx, MAX_CLIP);
            }
            if (id >= HIT_PLACE && id - HIT_PLACE < nplaces) {
                drop(fp, idx, n, places[id - HIT_PLACE], (mods & KEYS_CTRL) != 0);
            } else if (id >= HIT_AREA && id < HIT_AREA + 2 && id - HIT_AREA != from_pane) {
                drop(fp, idx, n, tabs[cur_tab].pane[id - HIT_AREA].cwd, (mods & KEYS_CTRL) != 0);
            } else if (id >= HIT_FILE && id < HIT_AREA) {
                int tp = (id - HIT_FILE) / HIT_PANE, ti = (id - HIT_FILE) % HIT_PANE;
                struct pane *to = &tabs[cur_tab].pane[tp];
                if (ti < to->nlist && to->list[ti].dir && !is_trash(to) && (tp != from_pane || ti != item)) {
                    char dir[PATH_MAX];
                    if (item_path(to, ti, dir, sizeof dir) == 0) {
                        drop(fp, idx, n, dir, (mods & KEYS_CTRL) != 0);
                    }
                } else if (tp != from_pane) {
                    drop(fp, idx, n, to->cwd, (mods & KEYS_CTRL) != 0);
                }
            }
        }
        return -1;
    }

    if (!e->pressed) return 0;
    drag_stop();
    int id = ui_hit_at(e->col, e->row);
    if (e->button == BTN_MIDDLE && id >= HIT_TAB && id - HIT_TAB < ntabs) {
        if (!pick_type) close_tab(id - HIT_TAB);
        return -1;
    }
    if (e->button != BTN_LEFT && e->button != BTN_RIGHT) return 0;
    if (id >= HIT_KEY) {
        if (e->button != BTN_LEFT) return 0;
        int raw = id - HIT_KEY;
        *out_mods = (raw >> 16) & 0xF;
        return raw & 0xFFFF;
    }
    if (id >= HIT_VIEW && id - HIT_VIEW < VIEWS) {
        if (e->button == BTN_LEFT) {
            P->view = id - HIT_VIEW;
            save_prefs();
        }
        return -1;
    }
    if (id >= HIT_TAB && id - HIT_TAB < ntabs) {
        if (e->button == BTN_LEFT) switch_tab(id - HIT_TAB);
        return -1;
    }
    if (id >= HIT_SORT && id < HIT_TAB) {
        int which = (id - HIT_SORT) / 16, col_sort = (id - HIT_SORT) % 16;
        tabs[cur_tab].active = which;
        side = 0;
        set_active();
        if (e->button == BTN_RIGHT) {
            draw();
            columns_menu(e->col + 1, e->row + 1);
            return -1;
        }
        apply_sort_choice(col_sort);
        return -1;
    }
    if (id >= HIT_PATH && id < HIT_SORT) {
        if (e->button != BTN_LEFT) return 0;
        char up[PATH_MAX], keep[256] = "";
        int k = id - HIT_PATH;
        if (k == 0) {
            const char *below = P->cwd + 1, *end = strchr(below, '/');
            snprintf(keep, sizeof keep, "%.*s", end ? (int)(end - below) : (int)strlen(below), below);
            go("/", keep);
        } else if (k < (int)strlen(P->cwd)) {
            const char *below = P->cwd + k + 1, *end = strchr(below, '/');
            snprintf(keep, sizeof keep, "%.*s", end ? (int)(end - below) : (int)strlen(below), below);
            snprintf(up, sizeof up, "%.*s", k, P->cwd);
            go(up, keep);
        }
        return -1;
    }
    if (id >= HIT_PLACE && id - HIT_PLACE < nplaces) {
        side_sel = id - HIT_PLACE;
        side = 1;
        if (e->button == BTN_LEFT) {
            place_go(side_sel);
            side = 0;
        }
        return -1;
    }
    if (id >= HIT_FILE && id < HIT_AREA) {
        int which = (id - HIT_FILE) / HIT_PANE, i = (id - HIT_FILE) % HIT_PANE;
        tabs[cur_tab].active = which;
        side = 0;
        set_active();
        if (i >= P->nlist) return 0;
        if (e->button == BTN_RIGHT) {
            P->sel = i;
            draw();
            int raw = file_menu(e->col, e->row, 1);
            if (raw < 0) return -1;
            *out_mods = raw == 'D' ? KEYS_SHIFT : ((raw >> 16) & 0xF);
            return raw & 0xFFFF;
        }
        int clear = 0;
        if (mods & KEYS_SHIFT) {
            for (int k = P->sel < i ? P->sel : i; k <= (P->sel < i ? i : P->sel); k++) P->list[k].marked = 1;
        } else if (mods & KEYS_CTRL) {
            P->list[i].marked = !P->list[i].marked;
        } else if (!P->list[i].marked && marked(P)) {
            for (int k = 0; k < P->nlist; k++) P->list[k].marked = 0;
        } else if (P->list[i].marked && e->clicks != 2) {
            clear = 1; /* cleared on release if it was not a drag */
        }
        P->sel = i;
        msg_sticky = 0;
        if (e->clicks == 2 && !(mods & (KEYS_SHIFT | KEYS_CTRL))) {
            drag_stop();
            return '\n';
        }
        int pcol, prow, pcols, pnrows;
        pane_area(P, which, &pcol, &prow, &pcols, &pnrows);
        int name_end = pcol + 4 + utf_cells(shown_name(&P->list[i]), 0, strlen(shown_name(&P->list[i])));
        if (P->view == VIEW_DETAILS && e->col > name_end) {
            id = HIT_AREA + which; /* blank space to the right of the name: rubberband */
        } else {
            drag_stop();
            drag.mode = DRAG_PENDING;
            drag.pane = which;
            drag.item = i;
            drag.ax = e->col;
            drag.ay = e->row;
            drag.col = e->col;
            drag.row = e->row;
            drag.mods = mods;
            drag.clear = clear;
            return -1;
        }
    }
    if (id >= HIT_AREA && id < HIT_AREA + 2) {
        int which = id - HIT_AREA;
        tabs[cur_tab].active = which;
        side = 0;
        set_active();
        if (e->button == BTN_RIGHT) {
            draw();
            int raw = file_menu(e->col, e->row, 0);
            if (raw < 0) return -1;
            *out_mods = raw == 'D' ? KEYS_SHIFT : ((raw >> 16) & 0xF);
            return raw & 0xFFFF;
        }
        int col, row, cols, nrows, gc, tc, tr, vr;
        pane_area(P, which, &col, &row, &cols, &nrows);
        pane_grid(P, cols, nrows, &gc, &tc, &tr, &vr);
        drag_stop();
        if (!(mods & (KEYS_SHIFT | KEYS_CTRL))) {
            for (int k = 0; k < P->nlist; k++) P->list[k].marked = 0;
        }
        drag.mode = DRAG_BAND;
        drag.pane = which;
        drag.mods = mods;
        drag.ax = px - col * cell_w;
        drag.ay = py - (top_px + row * cell_h) + (P->top / gc) * tr * cell_h;
        drag.x = px;
        drag.y = py;
        drag.col = e->col;
        drag.row = e->row;
        if (P->nlist > 0 && (drag.base = calloc((size_t)P->nlist, 1)) != NULL) {
            drag.nbase = P->nlist;
            for (int k = 0; k < P->nlist; k++) drag.base[k] = (char)P->list[k].marked;
        }
        band_mark();
        return -1;
    }
    return 0;
}

/* Until q (files_view), or a file chosen into out (files_pick: 1; q: 0). */
static int browse(char *out, size_t size)
{
    int show = 1;
    for (;;) {
        set_active();
        if (show) draw();
        show = 1;
        int key = key_poll_timeout(-1, drag.mode == DRAG_BAND ? 40 : 2000);
        int mods = keys_modifiers();
        if (key == K_POINTER) {
            mods = 0;
            key = pointer_key(&mods);
            if (key <= 0) {
                show = key < 0;
                continue;
            }
        }
        if (key == 0 && drag.mode == DRAG_BAND) {
            show = band_autoscroll();
            continue;
        }
        if (key == K_SHOWN || key == 0) {
            if ((pick_type || !take_request()) && key == K_SHOWN) {
                reload_all();
            }
            continue;
        }
        drag_stop();
        /* View modes, zoom, split, tabs, navigation, sorting: available while picking too. */
        if (key == K_CTRL_0 + 1 || key == K_CTRL_0 + 2 || key == K_CTRL_0 + 3) {
            P->view = key - (K_CTRL_0 + 1);
            save_prefs();
            continue;
        }
        if ((mods & (KEYS_CTRL | KEYS_ALT)) == (KEYS_CTRL | KEYS_ALT) && key >= '1' && key <= '5') {
            static const int pcmanfm_sorts[5] = { SORT_NAME, SORT_MTIME, SORT_SIZE, SORT_TYPE, SORT_EXT };
            apply_sort_choice(pcmanfm_sorts[key - '1']);
            continue;
        }
        if (key == 's') {
            int pcol, prow, pcols, pnrows;
            pane_area(P, tabs[cur_tab].active, &pcol, &prow, &pcols, &pnrows);
            sort_menu(pcol + 2, prow + 1);
            continue;
        }
        if (key == 'C') {
            int pcol, prow, pcols, pnrows;
            pane_area(P, tabs[cur_tab].active, &pcol, &prow, &pcols, &pnrows);
            columns_menu(pcol + 2, prow + 1);
            continue;
        }
        if (key == K_F1 + 8) { /* F9: toggle Places side pane (PCManFM) */
            side_hidden = !side_hidden;
            if (side_hidden) side = 0;
            save_prefs();
            continue;
        }
        if (key == K_CTRL_0 || ((mods & KEYS_CTRL) && (key == '+' || key == '=' || key == '-' || key == '_'))) {
            zoom_pane(P, key == K_CTRL_0 ? 0 : (key == '-' || key == '_') ? -1 : 1);
            continue;
        }
        if (key == K_F1 + 4) { /* F5 */
            reload_all();
            continue;
        }
        if (key == K_F1 + 2) { /* F3 */
            toggle_split();
            continue;
        }
        if (!pick_type && key == K_F1 + 3) { /* F4: open current folder in terminal (PCManFM) */
            open_terminal_here();
            continue;
        }
        if (key == ('f' & 0x1F) && (mods & (KEYS_CTRL | KEYS_SHIFT)) == (KEYS_CTRL | KEYS_SHIFT)) {
            find_files();
            continue;
        }
        if (!pick_type && key == ('d' & 0x1F) && mods == KEYS_CTRL) {
            toggle_bookmark();
            continue;
        }
        if (!pick_type && key == ('c' & 0x1F) && (mods & (KEYS_CTRL | KEYS_SHIFT)) == (KEYS_CTRL | KEYS_SHIFT)) {
            copy_paths();
            continue;
        }
        if (!pick_type && key == ('i' & 0x1F) && (mods & (KEYS_CTRL | KEYS_SHIFT)) == (KEYS_CTRL | KEYS_SHIFT)) {
            invert_selection();
            continue;
        }
        if (key == ('t' & 0x1F) && (mods & KEYS_CTRL)) {
            new_tab();
            continue;
        }
        if (key == ('w' & 0x1F) && (mods & KEYS_CTRL)) {
            if (ntabs > 1) close_tab(cur_tab);
            else return 0;
            continue;
        }
        if (key == '\t' && (mods & KEYS_CTRL)) {
            switch_tab(cur_tab + ((mods & KEYS_SHIFT) ? -1 : 1));
            continue;
        }
        if ((mods & KEYS_ALT) && (key == K_LEFT || key == K_RIGHT)) {
            history(key == K_RIGHT);
            continue;
        }
        if (((mods & KEYS_ALT) && key == K_UP) || key == '\b') {
            go_up();
            continue;
        }
        if (key == ('l' & 0x1F) && (mods & KEYS_CTRL)) {
            const char *loc = prompt(_("Location:"), P->cwd, 1);
            if (loc && loc[0]) {
                char exp[PATH_MAX];
                if (loc[0] == '~' && (loc[1] == '/' || loc[1] == '\0')) {
                    snprintf(exp, sizeof exp, "%s%s", home, loc + 1);
                    loc = exp;
                }
                go(loc, NULL);
            }
            continue;
        }
        if (key == '/' || (key == ('i' & 0x1F) && mods == KEYS_CTRL)) {
            const char *f = prompt(_("Filter (part of the name):"), P->filter, 0);
            snprintf(P->filter, sizeof P->filter, "%s", f ? f : "");
            load_dir(NULL);
            continue;
        }
        if (key == '.' || (key == ('h' & 0x1F) && (mods & KEYS_CTRL))) {
            char keep[256];
            snprintf(keep, sizeof keep, "%s", selected_name(P));
            hidden = !hidden;
            reload_all();
            load_dir(keep);
            continue;
        }
        if (key == 'i' || key == 'I' || ((mods & KEYS_ALT) && key == '\n')) {
            properties(key == 'I');
            continue;
        }
        if (key == ('a' & 0x1F) && (mods & KEYS_CTRL)) {
            for (int i = 0; i < P->nlist; i++) P->list[i].marked = 1;
            report(0, P_("Marked %d item.", "Marked all %d items.", P->nlist), P->nlist);
            continue;
        }
        if (key == 'q' || key == 27) {
            if (P->filter[0] && key == 27) {
                P->filter[0] = '\0';
                load_dir(NULL);
                continue;
            }
            return 0;
        }
        /* The home key (rc.conf key_home), as from any program: home's list,
         * this place and its tabs kept for the next time. Process 1 does
         * nothing with it here, the files being home's own. A program
         * choosing a file keeps its choice open. */
        if (key == K_ACT_HOME && !pick_type) {
            return 0;
        }
        if (pick_type && ((key > 0 && key < 0x80 && strchr(picking_folder() ? "xcvnNLVrdDweu " : "oxcvnNLVrdDweu ", key)) ||
                          key == K_DEL || key == K_F1 + 1 || key == K_F1 + 9 || (mods & KEYS_CTRL))) {
            continue; /* choosing: nothing is changed (o chooses a folder) */
        }
        if (key == '\t') {
            if (side_hidden) {
                if (tabs[cur_tab].split) tabs[cur_tab].active = !tabs[cur_tab].active;
            } else if (tabs[cur_tab].split) {
                int pos = side ? 0 : tabs[cur_tab].active + 1;
                pos = (pos + ((mods & KEYS_SHIFT) ? 2 : 1)) % 3;
                side = pos == 0;
                if (!side) tabs[cur_tab].active = pos - 1;
            } else {
                side = !side;
            }
        } else if (side && !side_hidden) {
            if (key == K_UP && side_sel > 0) {
                side_sel--;
            } else if (key == K_DOWN && side_sel < nplaces - 1) {
                side_sel++;
            } else if (key == '\n' || key == K_RIGHT) {
                place_go(side_sel);
                side = 0;
            }
        } else if (key == K_UP || key == K_DOWN || key == K_PGUP || key == K_PGDN ||
                   key == K_BOL || key == K_EOL ||
                   (P->view != VIEW_DETAILS && (key == K_LEFT || key == K_RIGHT))) {
            int col, row, cols, nrows, gc, tc, tr, vr;
            pane_area(P, tabs[cur_tab].active, &col, &row, &cols, &nrows);
            pane_grid(P, cols, nrows, &gc, &tc, &tr, &vr);
            int step = key == K_LEFT ? -1 : key == K_RIGHT ? 1 : key == K_UP ? -gc : key == K_DOWN ? gc :
                       key == K_PGUP ? -vr * gc : vr * gc;
            int next = key == K_BOL ? 0 : key == K_EOL ? P->nlist - 1 : P->sel + step;
            next = clampi(next, 0, P->nlist > 0 ? P->nlist - 1 : 0);
            if (mods & KEYS_SHIFT) {
                for (int k = P->sel < next ? P->sel : next; k <= (P->sel < next ? next : P->sel); k++) {
                    P->list[k].marked = 1;
                }
            }
            P->sel = next;
            msg_sticky = 0;
        } else if (key == K_LEFT && P->view == VIEW_DETAILS) {
            go_up();
        } else if ((key == '\n' || (key == K_RIGHT && P->view == VIEW_DETAILS) || key == 'o') && P->nlist > 0) {
            char path[PATH_MAX];
            if (is_trash(P)) {
                /* TRANSLATORS: r and i are keys; keep them. %s is a file name. */
                report(0, _("In the Trash: r restores %s, i shows its properties."), shown_name(&P->list[P->sel]));
            } else if (item_path(P, P->sel, path, sizeof path) < 0) {
                report(1, _("The path is too long."));
            } else if (P->list[P->sel].dir && key != 'o') {
                go(path, NULL);
            } else if (pick_type) {
                char type[OPEN_TYPE];
                as_user(1);
                int ok = open_type(path, type, sizeof type) == 0;
                as_user(0);
                if (!ok || strncmp(type, pick_type, strlen(pick_type)) != 0) {
                    /* TRANSLATORS: first %s is a file name, second %s is a MIME type prefix (e.g. "image/"). */
                    report(1, _("%s cannot be chosen here: not %s*."), P->list[P->sel].name, pick_type);
                } else {
                    snprintf(out, size, "%s", path);
                    return 1;
                }
            } else if (!P->list[P->sel].dir) {
                open_file(path, key == 'o', NULL);
            }
        } else if (key == ' ' && P->nlist > 0) {
            P->list[P->sel].marked = !P->list[P->sel].marked;
            P->sel = P->sel < P->nlist - 1 ? P->sel + 1 : P->sel;
        } else if ((key == 'c' || key == 'x' ||
                    ((mods & KEYS_CTRL) && (key == ('c' & 0x1F) || key == ('x' & 0x1F)))) && P->nlist > 0) {
            to_clip(key == 'x' || key == ('x' & 0x1F));
        } else if ((key == 'v' || ((mods & KEYS_CTRL) && key == ('v' & 0x1F))) && nclip) {
            paste();
        } else if (key == 'u' || ((mods & KEYS_CTRL) && key == ('z' & 0x1F))) {
            undo_last();
        } else if (key == 'n' || key == K_F1 + 9 ||
                   ((mods & (KEYS_CTRL | KEYS_SHIFT)) == (KEYS_CTRL | KEYS_SHIFT) && !(mods & KEYS_ALT) && key == ('n' & 0x1F))) {
            new_folder();
        } else if (key == 'N' ||
                   ((mods & (KEYS_CTRL | KEYS_ALT)) == (KEYS_CTRL | KEYS_ALT) && key == ('n' & 0x1F))) {
            new_empty_file();
        } else if (key == 'L' && P->nlist > 0) {
            create_symlink();
        } else if (key == ('u' & 0x1F) && mods == KEYS_CTRL && P->nlist > 0) {
            duplicate_selected();
        } else if ((key == 'r' || key == K_F1 + 1) && P->nlist > 0) {
            if (is_trash(P) && key == 'r') restore_targets();
            else rename_selected();
        } else if ((key == 'D' || ((key == 'd' || key == K_DEL) && (mods & KEYS_SHIFT))) && P->nlist > 0) {
            delete_targets();
        } else if ((key == 'd' || key == K_DEL) && P->nlist > 0) {
            if (is_trash(P)) {
                delete_targets();
            } else {
                int idx[MAX_CLIP], n = targets(P, idx, MAX_CLIP);
                trash_items(P, idx, n);
            }
        } else if (key >= KEY_EXTRACT_HERE && key <= KEY_COMPRESS_TO && P->nlist > 0) {
            archive_action(key);
        } else if (key == 'w' && P->nlist > 0 && !P->list[P->sel].dir && !is_trash(P)) {
            char path[PATH_MAX], type[128];
            as_user(1);
            int ok = item_path(P, P->sel, path, sizeof path) == 0 && open_type(path, type, sizeof type) == 0;
            as_user(0);
            if (!ok || strncmp(type, "image/", 6) != 0) {
                /* TRANSLATORS: w is the key; keep it. %s is a file name. */
                report(1, _("w makes an image the wallpaper; %s is not one."), P->list[P->sel].name);
            } else if (rc_set("wallpaper", path) < 0) {
                report(1, _("Cannot write %s."), RC_CONF);
            } else {
                report(0, _("Wallpaper: %s; the shared display shows it when it is next shown."),
                       P->list[P->sel].name);
            }
        } else if (key == 'e') {
            char drives[PATH_MAX], path[PATH_MAX];
            if (is_trash(P)) {
                empty_trash();
            } else if (P->nlist > 0 && P->list[P->sel].dir && join(drives, sizeof drives, home, "Drives") == 0 &&
                       strcmp(P->cwd, drives) == 0 && item_path(P, P->sel, path, sizeof path) == 0) {
                eject(path, P->list[P->sel].name);
            } else {
                /* TRANSLATORS: e is the key and Drives is the folder name ~/Drives; keep them. */
                report(1, _("e ejects a drive in Drives (or empties the Trash)."));
            }
        }
    }
}

void files_view(void)
{
    if (setup() < 0) {
        return;
    }
    if (!P->cwd[0] || (!is_trash(P) && !is_search(P) && !inside(P->cwd, home)) || go_pane(P, P->cwd, NULL, 0) < 0) {
        go_pane(P, home, NULL, 0);
    }
    take_request();
    browse(NULL, 0);
    drag_stop();
}

int files_pick(const char *dir, const char *keep, const char *type, const char *what, char *out, size_t size)
{
    if (setup() < 0) {
        return 0;
    }
    if (go(dir, keep) < 0) {
        go(home, NULL);
    }
    pick_type = type;
    report(0, "%s", what);
    int chosen = browse(out, size);
    pick_type = NULL;
    drag_stop();
    return chosen;
}
