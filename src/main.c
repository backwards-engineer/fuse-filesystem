#define FUSE_USE_VERSION 31
#include <fuse3/fuse.h>
#include "fs_core.h"
#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <unistd.h>

static int attributes(const char *path, struct stat *result, struct fuse_file_info *file) {
    (void)file;
    enum fs_kind kind = fs_lookup(path);
    if (kind == FS_MISSING) return -ENOENT;
    memset(result, 0, sizeof(*result));
    result->st_uid = getuid();
    result->st_gid = getgid();
    result->st_mode = kind == FS_DIRECTORY ? S_IFDIR | 0555 : S_IFREG | 0444;
    result->st_nlink = kind == FS_DIRECTORY ? 2 : 1;
    result->st_size = kind == FS_FILE ? (off_t)fs_file_size() : 0;
    return 0;
}

static int directory(const char *path, void *buffer, fuse_fill_dir_t emit,
                     off_t offset, struct fuse_file_info *file, enum fuse_readdir_flags flags) {
    (void)file;
    (void)flags;
    enum fs_kind kind = fs_lookup(path);
    if (kind == FS_MISSING) return -ENOENT;
    if (kind != FS_DIRECTORY) return -ENOTDIR;
    if (offset < 0) return -EINVAL;
    const char *entries[] = { ".", "..", "hello.txt" };
    for (off_t i = offset; i < 3; ++i) {
        if (emit(buffer, entries[i], NULL, i + 1, 0)) break;
    }
    return 0;
}

static int open_file(const char *path, struct fuse_file_info *file) {
    enum fs_kind kind = fs_lookup(path);
    if (kind == FS_MISSING) return -ENOENT;
    if (kind == FS_DIRECTORY) return -EISDIR;
    if ((file->flags & O_ACCMODE) != O_RDONLY || (file->flags & O_TRUNC)) return -EROFS;
    return 0;
}

static int read_file(const char *path, char *buffer, size_t count,
                     off_t offset, struct fuse_file_info *file) {
    (void)file;
    return fs_read(path, buffer, count, offset);
}

int main(int argc, char **argv) {
    const struct fuse_operations operations = {
        .getattr = attributes, .readdir = directory, .open = open_file, .read = read_file
    };
    return fuse_main(argc, argv, &operations, NULL);
}
