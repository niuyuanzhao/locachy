/* edit's projects (edit-tree.c), as Kate's Projects plugin finds and lists
 * them, read with the user's permissions. */
#ifndef LOCACHY_EDIT_TREE_H
#define LOCACHY_EDIT_TREE_H

#include <limits.h>

struct node {
    char *name;
    int dir, expanded, loaded;
    struct node *parent, **kids;
    int nkids;
};

struct project {
    char root[PATH_MAX];
    int vcs;              /* under version control (.git, .svn, .hg) */
    int git;              /* its files are git's (ls-files), else the folder's */
    struct node *top;     /* the root folder */
};

/* The project of a file: an open one above it (one under version control,
 * or the file's own folder's), else the nearest folder above it with .git
 * (a folder, or a file for a work tree), .svn or .hg, else the file's own
 * folder. NULL if there is no memory. */
struct project *project_for(const char *path);
void projects_free(void);

/* A separate event fd for project directories (never shares document watches).
 * Drain it after polling; project_refresh updates loaded directories while
 * keeping surviving nodes and expanded folders. */
int projects_poll_fd(void);
int projects_changed(void);
void project_refresh(struct project *p);

/* A folder's entries read when it is first opened. */
void node_expand(struct project *p, struct node *n);
/* The node of path in p, its folders opened down to it; NULL if not there. */
struct node *node_find(struct project *p, const char *path);
/* The path of node n. */
void node_path(const struct project *p, const struct node *n, char *out, size_t size);

/* The rows shown: the open folders' entries, depth first (not the root). */
int tree_rows(struct project *p, struct node **rows, int *depth, int max);

/* Every file of p as the tree lists them (git's files; else the folders'
 * regular files without hidden ones, read as the user, not descending into
 * links to folders), as malloc'd absolute paths in tree order, at most max;
 * *truncated set when there were more. The count, or -1 without memory.
 * The tree's own folders are neither opened nor read by it. */
int project_files(struct project *p, char ***paths, int max, int *truncated);
void project_files_free(char **paths, int n);

#endif
