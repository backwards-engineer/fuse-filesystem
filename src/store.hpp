#pragma once
#include <cstdint>
#include <functional>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <vector>

namespace localfs {
using Id = std::int64_t;
struct Error : std::runtime_error {
    int code;
    Error(int number, const std::string &message) : std::runtime_error(message), code(number) {}
};
struct Version {
    Id id, size, created_ns;
};
struct Entry {
    std::string name;
    Id inode;
};
struct Stats {
    Id inodes{}, versions{}, chunks{}, references{}, plaintext_bytes{}, stored_bytes{};
    std::size_t chunk_size{}, cache_bytes{}, cache_hits{}, cache_misses{};
};
class Store {
  public:
    static void initialize(const std::string &directory, const std::string &key_file,
                           std::size_t chunk_size = 65536, int compression = 3);
    Store(const std::string &directory, const std::string &key_file,
          std::size_t cache_limit = 64 * 1024 * 1024);
    ~Store();
    Store(const Store &) = delete;
    Store &operator=(const Store &) = delete;
    Id lookup(std::string_view path);
    Id lookup_at(Id parent, std::string_view name);
    Id create_at(Id parent, std::string_view name, mode_t mode, uid_t uid, gid_t gid,
                 std::string_view target = {});
    void remove_at(Id parent, std::string_view name, bool directory);
    void rename_at(Id parent, std::string_view name, Id new_parent, std::string_view new_name,
                   unsigned flags);
    struct stat stat(Id inode);
    std::vector<Entry> list(Id inode);
    Id create(std::string_view path, mode_t mode, uid_t uid, gid_t gid, std::string_view target = {});
    void remove(std::string_view path, bool directory);
    void rename(std::string_view from, std::string_view to, unsigned flags = 0);
    std::string readlink(Id inode);
    std::size_t read(Id inode, void *buffer, std::size_t size, Id offset, Id version = 0);
    std::size_t write(Id inode, const void *buffer, std::size_t size, Id offset, bool append = false);
    void truncate(Id inode, Id size);
    void chmod(Id inode, mode_t mode);
    void chown(Id inode, uid_t uid, gid_t gid);
    void utimens(Id inode, const struct timespec times[2]);
    Id seek(Id inode, Id offset, int whence);
    void punch_hole(Id inode, Id offset, Id length);
    void sync();
    struct statvfs space();
    std::vector<Version> history(Id inode);
    void restore(Id inode, Id version);
    Id prune(std::size_t keep);
    Id gc(bool purge_deleted = false);
    Stats stats();
    std::vector<std::string> check();
    // Test injection only; the executables never set this hook.
    void set_commit_hook(std::function<void(std::string_view)> hook);

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace localfs
