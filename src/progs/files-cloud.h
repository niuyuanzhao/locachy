#ifndef LOCACHY_FILES_CLOUD_H
#define LOCACHY_FILES_CLOUD_H

#include <stdint.h>

#define CLOUD_LIST_MAGIC 0x434c4431u
#define CLOUD_LIST_MAX 100000u
struct cloud_entry {
    uint32_t magic, mode, uid, gid, link, dir;
    int64_t size, blocks, mtime;
    char name[256], type[128];
};

#endif
