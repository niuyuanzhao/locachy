/* Directory metadata only, as the user. files keeps its UI responsive while
 * a cloud filesystem waits on its network. stdout is a private result memfd. */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <gio/gio.h>

#include "files-cloud.h"

static int write_all(const void *data, size_t size)
{
    const char *p = data;
    while (size) {
        ssize_t n = write(STDOUT_FILENO, p, size);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return -1;
        p += n;
        size -= (size_t)n;
    }
    return 0;
}

int main(int argc, char **argv)
{
    if (argc != 2 || geteuid() == 0) return 2;
    DIR *dir = opendir(argv[1]);
    if (!dir) return 1;
    struct dirent *entry;
    unsigned int count = 0;
    int failed = 0;
    for (;;) {
        errno = 0;
        entry = readdir(dir);
        if (!entry) {
            failed = errno != 0;
            break;
        }
        if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, "..")) continue;
        if (++count > CLOUD_LIST_MAX || strlen(entry->d_name) >= sizeof ((struct cloud_entry *)0)->name) {
            failed = 1;
            break;
        }
        struct stat st, lst;
        int fd = dirfd(dir);
        if (fstatat(fd, entry->d_name, &lst, AT_SYMLINK_NOFOLLOW) < 0) {
            if (errno == ENOENT) continue;
            failed = 1;
            break;
        }
        int link = S_ISLNK(lst.st_mode);
        if (fstatat(fd, entry->d_name, &st, 0) < 0) st = lst;
        struct cloud_entry row = { .magic = CLOUD_LIST_MAGIC, .mode = link ? lst.st_mode : st.st_mode,
            .uid = st.st_uid, .gid = st.st_gid, .link = link, .dir = S_ISDIR(st.st_mode), .size = st.st_size,
            .blocks = st.st_blocks, .mtime = st.st_mtime };
        snprintf(row.name, sizeof row.name, "%s", entry->d_name);
        gchar *type = row.dir ? NULL : g_content_type_guess(entry->d_name, NULL, 0, NULL);
        snprintf(row.type, sizeof row.type, "%s", row.dir ? "inode/directory" : type ? type : "application/octet-stream");
        g_free(type);
        if (write_all(&row, sizeof row) < 0) {
            failed = 1;
            break;
        }
    }
    if (closedir(dir) != 0) failed = 1;
    return failed;
}
