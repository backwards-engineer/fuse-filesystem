#include "fs_core.h"
#include <errno.h>
#include <limits.h>
#include <string.h>

static const char greeting[] = "Hello from local-fusefs!\n";

enum fs_kind fs_lookup(const char *path) {
    if (strcmp(path, "/") == 0) return FS_DIRECTORY;
    if (strcmp(path, "/hello.txt") == 0) return FS_FILE;
    return FS_MISSING;
}

size_t fs_file_size(void) { return sizeof(greeting) - 1; }

int fs_read(const char *path, char *buffer, size_t capacity, int64_t offset) {
    enum fs_kind kind = fs_lookup(path);
    if (kind == FS_MISSING) return -ENOENT;
    if (kind == FS_DIRECTORY) return -EISDIR;
    if (offset < 0) return -EINVAL;
    if ((uint64_t)offset >= fs_file_size()) return 0;
    size_t count = fs_file_size() - (size_t)offset;
    if (count > capacity) count = capacity;
    if (count > INT_MAX) count = INT_MAX;
    if (count != 0) memcpy(buffer, greeting + (size_t)offset, count);
    return (int)count;
}
