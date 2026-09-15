#ifndef LOCAL_FS_CORE_H
#define LOCAL_FS_CORE_H
#include <stddef.h>
#include <stdint.h>
enum fs_kind { FS_MISSING, FS_DIRECTORY, FS_FILE };
enum fs_kind fs_lookup(const char *path);
size_t fs_file_size(void);
int fs_read(const char *path, char *buffer, size_t capacity, int64_t offset);
#endif
