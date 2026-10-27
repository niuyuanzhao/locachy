/*
 * edit's projects (edit-tree.h), as Kate's Projects plugin has them
 * (addons/project/kateprojectplugin.cpp, kateprojectworker.cpp): a file's
 * project is an open one above it, else the nearest folder above it under
 * version control (.git, .svn, .hg: Kate's automatic repositories, on by
 * default), else (LOCachy's plan) the file's own folder. A git project's
 * files are git's: git ls-files of what it tracks (with submodules) and of
 * what is new and not ignored, run as the user; other projects show their
 * folders, without hidden files, each folder read when it is opened.
 * Folders come first, then files, each by name without regard to case.
 * Everything is read with the user's permissions.
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <dirent.h>
#include <fcntl.h>
#include <grp.h>
#include <pwd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/inotify.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include "edit-file.h"
#include "edit-tree.h"

#ifndef EDIT_GIT
#define EDIT_GIT "/usr/bin/git"
#endif
#define MAX_PROJECTS 32

static struct project *projects[MAX_PROJECTS];
static int nprojects;
static int project_fd = -1;

int projects_poll_fd(void)
{
    if (project_fd < 0) {
        project_fd = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
    }
    return project_fd;
}

int projects_changed(void)
{
    char buf[8192];
    int changed = 0;
    while (project_fd >= 0 && read(project_fd, buf, sizeof buf) > 0) {
        changed = 1;
    }
    return changed;
}

static void watch_folder(const char *path)
{
    int fd = projects_poll_fd();
    if (fd >= 0) {
        inotify_add_watch(fd, path, IN_CREATE | IN_DELETE | IN_MOVED_FROM | IN_MOVED_TO |
                                   IN_DELETE_SELF | IN_MOVE_SELF | IN_ATTRIB);
    }
}

static struct node *new_node(struct node *parent, const char *name, size_t len, int dir)
{
    struct node *n = calloc(1, sizeof *n);
    if (!n || !(n->name = strndup(name, len))) {
        free(n);
        return NULL;
    }
    n->dir = dir;
    n->parent = parent;
    if (parent) {
        struct node **kids = realloc(parent->kids, (size_t)(parent->nkids + 1) * sizeof *kids);
        if (!kids) {
            free(n->name);
            free(n);
            return NULL;
        }
        parent->kids = kids;
        kids[parent->nkids++] = n;
    }
    return n;
}

static void free_node(struct node *n)
{
    for (int i = 0; i < n->nkids; i++) {
        free_node(n->kids[i]);
    }
    free(n->kids);
    free(n->name);
    free(n);
}

void projects_free(void)
{
    for (int i = 0; i < nprojects; i++) {
        if (projects[i]->top) {
            free_node(projects[i]->top);
        }
        free(projects[i]);
    }
    nprojects = 0;
    if (project_fd >= 0) {
        close(project_fd);
        project_fd = -1;
    }
}

static int by_kind_and_name(const void *a, const void *b)
{
    const struct node *x = *(struct node *const *)a, *y = *(struct node *const *)b;
    if (x->dir != y->dir) {
        return y->dir - x->dir;
    }
    return strcasecmp(x->name, y->name);
}

static void sort_tree(struct node *n)
{
    if (n->nkids > 1) {
        qsort(n->kids, (size_t)n->nkids, sizeof *n->kids, by_kind_and_name);
    }
    for (int i = 0; i < n->nkids; i++) {
        if (n->kids[i]->dir) {
            sort_tree(n->kids[i]);
        }
    }
}

void node_path(const struct project *p, const struct node *n, char *out, size_t size)
{
    if (!n || !n->parent) {
        snprintf(out, size, "%s", p->root);
        return;
    }
    char up[PATH_MAX];
    node_path(p, n->parent, up, sizeof up);
    snprintf(out, size, "%s/%s", up, n->name);
}

void node_expand(struct project *p, struct node *n)
{
    if (!n->dir) {
        return;
    }
    n->expanded = 1;
    if (n->loaded) {
        return;
    }
    n->loaded = 1;
    char path[PATH_MAX];
    node_path(p, n, path, sizeof path);
    file_as_user(1);
    DIR *d = opendir(path);
    if (d) {
        watch_folder(path);
    }
    struct dirent *e;
    while (d && (e = readdir(d)) != NULL) {
        if (e->d_name[0] == '.') {
            continue; /* hidden files: Kate's default leaves them out */
        }
        char full[PATH_MAX];
        struct stat st;
        if (snprintf(full, sizeof full, "%s/%s", path, e->d_name) >= (int)sizeof full || stat(full, &st) < 0) {
            continue;
        }
        if (S_ISDIR(st.st_mode) || S_ISREG(st.st_mode)) {
            new_node(n, e->d_name, strlen(e->d_name), S_ISDIR(st.st_mode));
        }
    }
    if (d) {
        closedir(d);
    }
    file_as_user(0);
    if (n->nkids > 1) {
        qsort(n->kids, (size_t)n->nkids, sizeof *n->kids, by_kind_and_name);
    }
}

/* The output of git ARGS in root, run as the user: malloc'd, or NULL. */
static char *git(const char *root, char *const args[], size_t *len)
{
    int fds[2];
    if (pipe2(fds, O_CLOEXEC) < 0) {
        return NULL;
    }
    const struct passwd *pw = getpwuid(EDIT_USER);
    char home[PATH_MAX + 8];
    snprintf(home, sizeof home, "HOME=%s", pw ? pw->pw_dir : "/");
    pid_t pid = fork();
    if (pid == 0) {
        int null = open("/dev/null", O_RDWR);
        dup2(null, 0);
        dup2(fds[1], 1);
        dup2(null, 2);
        close_range(3, ~0U, 0); /* none of edit's devices */
        char *env[] = { home, "PATH=/usr/local/bin:/usr/bin", "LC_ALL=C", NULL };
        if (setgroups(0, NULL) < 0 || setgid(EDIT_USER) < 0 || setuid(EDIT_USER) < 0 || chdir(root) < 0) {
            _exit(127);
        }
        execve(EDIT_GIT, args, env);
        _exit(127);
    }
    close(fds[1]);
    if (pid < 0) {
        close(fds[0]);
        return NULL;
    }
    size_t size = 65536, n = 0;
    char *out = malloc(size);
    for (;;) {
        if (out && n + 4096 > size) {
            char *more = realloc(out, size *= 2);
            if (!more) {
                free(out);
                out = NULL;
            } else {
                out = more;
            }
        }
        char sink[4096];
        ssize_t got = read(fds[0], out ? out + n : sink, out ? size - n : sizeof sink);
        if (got <= 0) {
            break;
        }
        n += out ? (size_t)got : 0;
    }
    close(fds[0]);
    int status;
    waitpid(pid, &status, 0);
    if (!out || !WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        free(out);
        return NULL;
    }
    *len = n;
    return out;
}

/* path (relative to the project) put in the tree as a file. */
static void add_path(struct node *top, const char *path)
{
    struct node *at = top;
    for (const char *p = path; *p;) {
        const char *slash = strchr(p, '/');
        size_t len = slash ? (size_t)(slash - p) : strlen(p);
        struct node *next = NULL;
        for (int i = 0; i < at->nkids && !next; i++) {
            if (strlen(at->kids[i]->name) == len && strncmp(at->kids[i]->name, p, len) == 0) {
                next = at->kids[i];
            }
        }
        if (!next && !(next = new_node(at, p, len, slash != NULL))) {
            return;
        }
        if (slash) {
            next->loaded = 1;
        }
        at = next;
        p = slash ? slash + 1 : p + len;
    }
}

/* Kate's git listing: what git tracks and what is new, not ignored. */
static int git_files(struct project *p)
{
    char *const tracked[] = { "git", "ls-files", "-t", "-z", "--recurse-submodules", "--deduplicate", ".", NULL };
    char *const others[] = { "git", "ls-files", "-t", "-z", "--others", "--exclude-standard", "--deduplicate", ".",
                             NULL };
    size_t n1, n2;
    char *a = git(p->root, tracked, &n1);
    if (!a) {
        return -1;
    }
    char *b = git(p->root, others, &n2);
    for (int pass = 0; pass < 2; pass++) {
        char *s = pass ? b : a;
        size_t n = pass ? n2 : n1;
        for (size_t i = 0; s && i < n;) {
            size_t len = strnlen(s + i, n - i);
            const char *entry = s + i;
            i += len + 1;
            /* "T path": the tag, of which S (not checked out) is left out */
            if (len < 3 || entry[1] != ' ' || entry[0] == 'S') {
                continue;
            }
            char full[PATH_MAX];
            struct stat st;
            size_t rootlen = strlen(p->root);
            if (len - 2 >= sizeof full || rootlen + 1 >= sizeof full - (len - 2)) continue;
            memcpy(full, p->root, rootlen);
            full[rootlen] = '/';
            memcpy(full + rootlen + 1, entry + 2, len - 2);
            full[rootlen + len - 1] = '\0';
            file_as_user(1);
            int there = stat(full, &st) == 0 && S_ISREG(st.st_mode);
            file_as_user(0);
            if (there) {
                char rel[PATH_MAX];
                snprintf(rel, sizeof rel, "%.*s", (int)(len - 2), entry + 2);
                add_path(p->top, rel);
            }
        }
    }
    free(a);
    free(b);
    sort_tree(p->top);
    return 0;
}

/* Reconcile a snapshot with existing nodes: pointers and expansion state of
 * surviving entries remain valid. Deleted entries are freed only here. */
static void merge_children(struct node *old, struct node *fresh, int recursive)
{
    for (int i = 0; i < fresh->nkids; i++) {
        struct node *next = fresh->kids[i];
        for (int j = 0; j < old->nkids; j++) {
            struct node *keep = old->kids[j];
            if (!keep || keep->dir != next->dir || strcmp(keep->name, next->name) != 0) {
                continue;
            }
            if (recursive && keep->dir) {
                merge_children(keep, next, 1);
            }
            free_node(next);
            fresh->kids[i] = keep;
            keep->parent = old;
            old->kids[j] = NULL;
            break;
        }
        fresh->kids[i]->parent = old;
    }
    for (int i = 0; i < old->nkids; i++) {
        if (old->kids[i]) free_node(old->kids[i]);
    }
    free(old->kids);
    old->kids = fresh->kids;
    old->nkids = fresh->nkids;
    fresh->kids = NULL;
    fresh->nkids = 0;
}

static void refresh_folder(struct project *p, struct node *n)
{
    if (!n->dir || !n->loaded) return;
    struct node *fresh = new_node(NULL, n->name, strlen(n->name), 1);
    if (!fresh) return;
    fresh->parent = n->parent;
    node_expand(p, fresh);
    merge_children(n, fresh, 0);
    fresh->parent = NULL;
    free_node(fresh);
    for (int i = 0; i < n->nkids; i++) refresh_folder(p, n->kids[i]);
}

/* Git lists files, not empty directories. Watch the actual directory tree
 * too, or a new file inside an empty/new directory can never invalidate it.
 * Do not follow directory symlinks (cycles) or watch repository metadata. */
static void watch_git_path(const char *path)
{
    watch_folder(path);
    DIR *d = opendir(path);
    struct dirent *e;
    while (d && (e = readdir(d)) != NULL) {
        if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0 ||
            strcmp(e->d_name, ".git") == 0 || strcmp(e->d_name, ".svn") == 0 ||
            strcmp(e->d_name, ".hg") == 0) continue;
        char child[PATH_MAX];
        struct stat st;
        if (snprintf(child, sizeof child, "%s/%s", path, e->d_name) >= (int)sizeof child) continue;
        if (lstat(child, &st) == 0 && S_ISDIR(st.st_mode)) watch_git_path(child);
    }
    if (d) closedir(d);
}

static void watch_git_folders(struct project *p, struct node *n)
{
    (void)n;
    file_as_user(1);
    watch_git_path(p->root);
    file_as_user(0);
}

void project_refresh(struct project *p)
{
    if (!p || !p->top) return;
    if (p->git) {
        struct project fresh = *p;
        fresh.top = new_node(NULL, p->top->name, strlen(p->top->name), 1);
        if (!fresh.top) return;
        if (git_files(&fresh) == 0) merge_children(p->top, fresh.top, 1);
        free_node(fresh.top);
        watch_git_folders(p, p->top);
    } else {
        refresh_folder(p, p->top);
    }
}

static int is_there(const char *dir, const char *name, int want_dir)
{
    char path[PATH_MAX];
    struct stat st;
    if (snprintf(path, sizeof path, "%s/%s", dir, name) >= (int)sizeof path) {
        return 0;
    }
    return lstat(path, &st) == 0 && (!want_dir || S_ISDIR(st.st_mode));
}

struct project *project_for(const char *path)
{
    char dir[PATH_MAX];
    snprintf(dir, sizeof dir, "%s", path);
    char *slash = strrchr(dir, '/');
    if (!slash) {
        return NULL;
    }
    *slash = '\0';
    if (!dir[0]) {
        snprintf(dir, sizeof dir, "/");
    }
    /* An open project above it first, the nearest (one of a folder without
     * version control only for the files in that folder). */
    char up[PATH_MAX];
    snprintf(up, sizeof up, "%s", dir);
    for (;;) {
        for (int i = 0; i < nprojects; i++) {
            if (strcmp(projects[i]->root, up) == 0 && (projects[i]->vcs || strcmp(up, dir) == 0)) {
                return projects[i];
            }
        }
        char *s = strrchr(up, '/');
        if (!s || s == up) {
            break;
        }
        *s = '\0';
    }
    if (nprojects == MAX_PROJECTS) {
        return NULL;
    }
    struct project *p = calloc(1, sizeof *p);
    if (!p) {
        return NULL;
    }
    snprintf(up, sizeof up, "%s", dir);
    file_as_user(1);
    for (;;) {
        if (is_there(up, ".git", 0) || is_there(up, ".svn", 1) || is_there(up, ".hg", 1)) {
            snprintf(p->root, sizeof p->root, "%s", up);
            p->vcs = 1;
            p->git = is_there(up, ".git", 0);
            break;
        }
        char *s = strrchr(up, '/');
        if (!s || s == up) {
            break;
        }
        *s = '\0';
    }
    file_as_user(0);
    if (!p->root[0]) {
        snprintf(p->root, sizeof p->root, "%s", dir);
    }
    const char *name = strrchr(p->root, '/');
    p->top = new_node(NULL, name && name[1] ? name + 1 : p->root, strlen(name && name[1] ? name + 1 : p->root), 1);
    if (!p->top) {
        free(p);
        return NULL;
    }
    if (p->git && git_files(p) == 0) {
        p->top->loaded = 1;
    } else {
        p->git = 0;
    }
    p->top->expanded = 1;
    if (p->git) watch_git_folders(p, p->top);
    projects[nprojects++] = p;
    return p;
}

struct node *node_find(struct project *p, const char *path)
{
    size_t n = strlen(p->root);
    if (strncmp(path, p->root, n) != 0 || path[n] != '/') {
        return NULL;
    }
    struct node *at = p->top;
    node_expand(p, at);
    for (const char *s = path + n + 1; *s;) {
        const char *slash = strchr(s, '/');
        size_t len = slash ? (size_t)(slash - s) : strlen(s);
        struct node *next = NULL;
        for (int i = 0; i < at->nkids && !next; i++) {
            if (strlen(at->kids[i]->name) == len && strncmp(at->kids[i]->name, s, len) == 0) {
                next = at->kids[i];
            }
        }
        if (!next) {
            return NULL;
        }
        if (slash) {
            node_expand(p, next);
        }
        at = next;
        s = slash ? slash + 1 : s + len;
    }
    return at;
}

static int rows_of(struct node *n, int level, struct node **rows, int *depth, int count, int max)
{
    for (int i = 0; i < n->nkids && count < max; i++) {
        rows[count] = n->kids[i];
        depth[count++] = level;
        if (n->kids[i]->dir && n->kids[i]->expanded) {
            count = rows_of(n->kids[i], level + 1, rows, depth, count, max);
        }
    }
    return count;
}

int tree_rows(struct project *p, struct node **rows, int *depth, int max)
{
    return p && p->top ? rows_of(p->top, 0, rows, depth, 0, max) : 0;
}

/* The files of a project, for Quick Open (Kate's projectFiles property). */
struct file_list {
    char **paths;
    int n, size, max, truncated;
};

static int list_add(struct file_list *l, const char *path)
{
    if (l->n == l->max) {
        l->truncated = 1;
        return 0;
    }
    if (l->n == l->size) {
        int size = l->size ? l->size * 2 : 256;
        char **more = realloc(l->paths, (size_t)size * sizeof *more);
        if (!more) {
            return -1;
        }
        l->paths = more;
        l->size = size;
    }
    if (!(l->paths[l->n] = strdup(path))) {
        return -1;
    }
    l->n++;
    return 1;
}

static int list_nodes(const struct project *p, const struct node *n, struct file_list *l)
{
    for (int i = 0; i < n->nkids; i++) {
        const struct node *k = n->kids[i];
        int r;
        if (k->dir) {
            r = list_nodes(p, k, l);
        } else {
            char path[PATH_MAX];
            node_path(p, k, path, sizeof path);
            r = list_add(l, path);
        }
        if (r <= 0) {
            return r;
        }
    }
    return 1;
}

/* A folder read as node_expand reads one (as the user, without hidden
 * entries, folders first), into a node of its own, then its folders. */
static int list_folder(const char *path, int depth, struct file_list *l)
{
    struct node top = { 0 };
    DIR *d = opendir(path);
    struct dirent *e;
    while (d && (e = readdir(d)) != NULL) {
        if (e->d_name[0] == '.') {
            continue;
        }
        char full[PATH_MAX];
        struct stat st;
        if (snprintf(full, sizeof full, "%s/%s", path, e->d_name) >= (int)sizeof full || stat(full, &st) < 0) {
            continue;
        }
        if (S_ISDIR(st.st_mode) || S_ISREG(st.st_mode)) {
            new_node(&top, e->d_name, strlen(e->d_name), S_ISDIR(st.st_mode));
        }
    }
    if (d) {
        closedir(d);
    }
    if (top.nkids > 1) {
        qsort(top.kids, (size_t)top.nkids, sizeof *top.kids, by_kind_and_name);
    }
    int r = 1;
    for (int i = 0; i < top.nkids && r > 0; i++) {
        char full[PATH_MAX];
        struct stat st;
        snprintf(full, sizeof full, "%s/%s", path, top.kids[i]->name);
        if (!top.kids[i]->dir) {
            r = list_add(l, full);
        } else if (depth < 64 && lstat(full, &st) == 0 && S_ISDIR(st.st_mode)) {
            r = list_folder(full, depth + 1, l); /* a link to a folder is not followed */
        }
    }
    for (int i = 0; i < top.nkids; i++) {
        free_node(top.kids[i]);
    }
    free(top.kids);
    return r;
}

int project_files(struct project *p, char ***paths, int max, int *truncated)
{
    struct file_list l = { .max = max };
    *paths = NULL;
    *truncated = 0;
    if (!p || !p->top) {
        return 0;
    }
    int r;
    if (p->git) {
        r = list_nodes(p, p->top, &l);
    } else {
        file_as_user(1);
        r = list_folder(p->root, 0, &l);
        file_as_user(0);
    }
    if (r < 0) {
        project_files_free(l.paths, l.n);
        return -1;
    }
    *paths = l.paths;
    *truncated = l.truncated;
    return l.n;
}

void project_files_free(char **paths, int n)
{
    for (int i = 0; i < n; i++) {
        free(paths[i]);
    }
    free(paths);
}
