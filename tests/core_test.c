#include "fs_core.h"
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define CHECK(expr) do { if (!(expr)) { fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #expr); return 1; } } while (0)
int main(void) {
    char buffer[128] = {0};
    CHECK(fs_lookup("/") == FS_DIRECTORY);
    CHECK(fs_lookup("/hello.txt") == FS_FILE);
    CHECK(fs_lookup("/hello.txt/child") == FS_MISSING);
    CHECK(fs_lookup("/missing") == FS_MISSING);
    CHECK(fs_read("/missing", buffer, sizeof(buffer), 0) == -ENOENT);
    CHECK(fs_read("/", buffer, sizeof(buffer), 0) == -EISDIR);
    CHECK(fs_read("/hello.txt", buffer, sizeof(buffer), -1) == -EINVAL);
    CHECK(fs_read("/hello.txt", buffer, sizeof(buffer), 0) == (int)fs_file_size());
    CHECK(strcmp(buffer, "Hello from local-fusefs!\n") == 0);
    CHECK(fs_read("/hello.txt", buffer, 4, 6) == 4);
    CHECK(memcmp(buffer, "from", 4) == 0);
    CHECK(fs_read("/hello.txt", buffer, sizeof(buffer), (int64_t)fs_file_size() - 1) == 1);
    CHECK(buffer[0] == '\n');
    CHECK(fs_read("/hello.txt", buffer, sizeof(buffer), (int64_t)fs_file_size()) == 0);
    CHECK(fs_read("/hello.txt", buffer, sizeof(buffer), INT64_MAX) == 0);
    CHECK(fs_read("/hello.txt", buffer, 0, 0) == 0);
    puts("All filesystem core checks passed.");
    return 0;
}
