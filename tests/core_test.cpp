#include "crypto.hpp"
#include "store.hpp"
#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <filesystem>
#include <functional>
#include <iostream>
#include <mutex>
#include <random>
#include <signal.h>
#include <sqlite3.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
#include <vector>

using namespace localfs;
#define CHECK(value)                                                                                         \
    do {                                                                                                     \
        if (!(value))                                                                                        \
            throw std::runtime_error(std::string(__FILE__) + ":" + std::to_string(__LINE__) + ": " #value);  \
    } while (0)
namespace {
struct Fixture {
    std::string root, directory, key;
    explicit Fixture(std::size_t chunk = 4096, int compression = 3) {
        char pattern[] = "/tmp/local-fusefs-test-XXXXXX";
        auto *result = mkdtemp(pattern);
        CHECK(result);
        root = result;
        directory = root + "/store";
        key = root + "/key";
        Store::initialize(directory, key, chunk, compression);
    }
    ~Fixture() {
        std::error_code ec;
        std::filesystem::remove_all(root, ec);
    }
};
template <class F> void expect(int error, F action) {
    try {
        action();
    } catch (const Error &e) {
        CHECK(e.code == error);
        return;
    }
    throw std::runtime_error("Expected errno " + std::to_string(error));
}
Id file(Store &s, const std::string &path) {
    return s.create(path, S_IFREG | 0640, getuid(), getgid());
}
Bytes read(Store &s, Id id, Id version = 0) {
    auto length = s.stat(id).st_size;
    if (version)
        for (auto v : s.history(id))
            if (v.id == version)
                length = v.size;
    Bytes out(static_cast<std::size_t>(length));
    CHECK(s.read(id, out.data(), out.size(), 0, version) == out.size());
    return out;
}
void write(Store &s, Id id, const Bytes &bytes, Id offset = 0) {
    CHECK(s.write(id, bytes.data(), bytes.size(), offset) == bytes.size());
}
void sql(const std::string &directory, const char *statement) {
    sqlite3 *db = nullptr;
    CHECK(sqlite3_open((directory + "/store.db").c_str(), &db) == SQLITE_OK);
    auto rc = sqlite3_exec(db, statement, nullptr, nullptr, nullptr);
    std::string message = sqlite3_errmsg(db);
    sqlite3_close(db);
    if (rc != SQLITE_OK)
        throw std::runtime_error(message);
}
void healthy(Store &s) {
    auto errors = s.check();
    for (auto &e : errors)
        std::cerr << e << '\n';
    CHECK(errors.empty());
}
void test_namespace() {
    Fixture f;
    Store s(f.directory, f.key);
    CHECK(S_ISDIR(s.stat(1).st_mode));
    auto dir = s.create("/dir", S_IFDIR | 0750, getuid(), getgid());
    auto id = file(s, "/dir/file");
    CHECK(s.lookup("/dir/file") == id);
    CHECK(s.list(dir).size() == 1);
    CHECK(s.stat(1).st_nlink == 3);
    expect(EEXIST, [&] { file(s, "/dir/file"); });
    expect(ENOTEMPTY, [&] { s.remove("/dir", true); });
    expect(ENOTDIR, [&] { file(s, "/dir/file/a"); });
    expect(ENOENT, [&] { s.lookup("/missing"); });
    expect(EBUSY, [&] { s.remove("/", true); });
    expect(EINVAL, [&] { s.lookup("/../a"); });
    expect(ENAMETOOLONG, [&] { file(s, "/" + std::string(256, 'x')); });
    s.remove("/dir/file", false);
    CHECK(s.stat(id).st_nlink == 0);
    write(s, id, Bytes{1, 2, 3});
    CHECK(read(s, id) == Bytes({1, 2, 3}));
    s.remove("/dir", true);
    CHECK(s.list(1).empty());
    healthy(s);
}
void test_chunk_sizes() {
    for (auto chunk : {4096U, 65536U, 1048576U, 4194304U}) {
        Fixture f(chunk);
        Store s(f.directory, f.key);
        auto id = file(s, "/data");
        Bytes input(chunk + 19);
        for (std::size_t i = 0; i < input.size(); ++i)
            input[i] = static_cast<unsigned char>(i * 17);
        write(s, id, input, 7);
        auto output = read(s, id);
        CHECK(output.size() == input.size() + 7);
        CHECK(std::equal(input.begin(), input.end(), output.begin() + 7));
        CHECK(std::all_of(output.begin(), output.begin() + 7, [](auto x) { return x == 0; }));
        CHECK(s.stats().chunk_size == chunk);
        healthy(s);
    }
    expect(EINVAL, [] { Store::initialize("/unused", "/unused-key", 5000, 3); });
}
void test_random_io_model() {
    Fixture f;
    Store s(f.directory, f.key);
    auto id = file(s, "/random");
    std::mt19937 random(7281);
    Bytes model;
    for (int operation = 0; operation < 250; ++operation) {
        if (random() % 5 == 0) {
            auto size = random() % 40000;
            s.truncate(id, size);
            model.resize(size, 0);
        } else {
            auto offset = random() % 40000;
            Bytes input(1 + random() % 6000);
            for (auto &b : input)
                b = static_cast<unsigned char>(random());
            write(s, id, input, offset);
            if (model.size() < offset + input.size())
                model.resize(offset + input.size(), 0);
            std::copy(input.begin(), input.end(), model.begin() + offset);
        }
        CHECK(read(s, id) == model);
    }
    char byte{};
    expect(EINVAL, [&] { s.read(id, &byte, 1, -1); });
    expect(EFBIG, [&] { s.write(id, &byte, 1, INT64_MAX); });
    CHECK(s.read(id, &byte, 1, INT64_MAX) == 0);
    healthy(s);
}
void test_sparse_and_holes() {
    Fixture f;
    Store s(f.directory, f.key);
    auto id = file(s, "/sparse");
    Id offset = Id{1} << 40;
    write(s, id, Bytes{7, 8, 9}, offset);
    CHECK(s.stat(id).st_size == offset + 3);
    CHECK(s.stat(id).st_blocks == 8);
    CHECK(s.stats().chunks == 1);
    CHECK(s.seek(id, 0, SEEK_DATA) == offset);
    CHECK(s.seek(id, 0, SEEK_HOLE) == 0);
    CHECK(s.seek(id, offset, SEEK_HOLE) == offset + 3);
    expect(ENXIO, [&] { s.seek(id, offset + 3, SEEK_DATA); });
    Bytes zero(100, 9);
    CHECK(s.read(id, zero.data(), zero.size(), 1000000) == 100);
    CHECK(std::all_of(zero.begin(), zero.end(), [](auto b) { return b == 0; }));
    s.punch_hole(id, offset, 4096);
    CHECK(s.stat(id).st_blocks == 0);
    CHECK(s.stat(id).st_size == offset + 3);
    expect(ENXIO, [&] { s.seek(id, 0, SEEK_DATA); });
    s.truncate(id, 3);
    write(s, id, Bytes{1, 2, 3, 4, 5}, 0);
    s.truncate(id, 2);
    s.truncate(id, 5);
    CHECK(read(s, id) == Bytes({1, 2, 0, 0, 0}));
    healthy(s);
}
void test_dedup_compression_cache() {
    Fixture f;
    Store s(f.directory, f.key, 8192);
    auto a = file(s, "/a"), b = file(s, "/b");
    Bytes data(8192, 42);
    write(s, a, data);
    write(s, b, data);
    auto st = s.stats();
    CHECK(st.chunks == 1);
    CHECK(st.references == 4);
    CHECK(st.stored_bytes < st.plaintext_bytes / 10);
    CHECK(read(s, a) == data);
    CHECK(read(s, b) == data);
    st = s.stats();
    CHECK(st.cache_hits > 0);
    CHECK(st.cache_bytes <= 8192);
    write(s, a, Bytes(4096, 43), 8192);
    read(s, a);
    write(s, a, Bytes(4096, 44), 12288);
    read(s, a);
    CHECK(s.stats().cache_bytes <= 8192);
    healthy(s);
    Fixture raw(4096, 0);
    Store no_compression(raw.directory, raw.key, 0);
    auto c = file(no_compression, "/c");
    write(no_compression, c, Bytes(4096, 1));
    CHECK(no_compression.stats().stored_bytes == 4124);
    read(no_compression, c);
    CHECK(no_compression.stats().cache_bytes == 0);
}
void test_history_gc_restore() {
    Fixture f;
    Store s(f.directory, f.key);
    auto a = file(s, "/a");
    write(s, a, Bytes(4096, 1));
    auto first = s.history(a).front().id;
    write(s, a, Bytes(4096, 2));
    auto second = s.history(a).front().id;
    CHECK(first != second);
    CHECK(read(s, a, first) == Bytes(4096, 1));
    s.restore(a, first);
    CHECK(read(s, a) == Bytes(4096, 1));
    CHECK(s.history(a).front().id > second);
    CHECK(s.gc() == 0);
    CHECK(s.prune(1) >= 3);
    CHECK(s.gc() == 1);
    expect(ENOENT, [&] { s.restore(a, second); });
    CHECK(read(s, a) == Bytes(4096, 1));
    s.remove("/a", false);
    CHECK(!s.history(a).empty());
    CHECK(s.gc() == 0);
    CHECK(s.gc(true) == 1);
    expect(ENOENT, [&] { s.stat(a); });
    healthy(s);
}
void test_permissions_symlinks_times() {
    Fixture f;
    Store s(f.directory, f.key);
    auto id = file(s, "/a");
    CHECK((s.stat(id).st_mode & 0777) == 0640);
    s.chmod(id, 04711);
    CHECK((s.stat(id).st_mode & 07777) == 04711);
    write(s, id, Bytes{1});
    CHECK((s.stat(id).st_mode & S_ISUID) == 0);
    s.chown(id, 123, 456);
    CHECK(s.stat(id).st_uid == 123 && s.stat(id).st_gid == 456);
    timespec times[2]{{100, 123}, {200, 456}};
    s.utimens(id, times);
    CHECK(s.stat(id).st_atim.tv_nsec == 123 && s.stat(id).st_mtim.tv_sec == 200);
    auto link = s.create("/link", S_IFLNK | 0777, getuid(), getgid(), "a");
    CHECK(s.readlink(link) == "a");
    CHECK(s.stat(link).st_size == 1);
    expect(EINVAL, [&] { s.readlink(id); });
    s.create("/group", S_IFDIR | 02770, 123, 456);
    auto child = file(s, "/group/file");
    CHECK(s.stat(child).st_gid == 456);
    healthy(s);
}
void test_rename() {
    Fixture f;
    Store s(f.directory, f.key);
    auto a = file(s, "/a"), b = file(s, "/b");
    write(s, a, Bytes{1});
    write(s, b, Bytes{2});
    expect(EEXIST, [&] { s.rename("/a", "/b", 1); });
    s.rename("/a", "/b", 2);
    CHECK(s.lookup("/a") == b && s.lookup("/b") == a);
    s.rename("/a", "/b");
    CHECK(s.lookup("/b") == b);
    CHECK(read(s, a) == Bytes({1}));
    CHECK(s.stat(a).st_nlink == 0);
    s.create("/dir", S_IFDIR | 0755, getuid(), getgid());
    s.create("/dir/sub", S_IFDIR | 0755, getuid(), getgid());
    expect(EINVAL, [&] { s.rename("/dir", "/dir/sub/dir"); });
    expect(EINVAL, [&] { s.rename("/dir", "/dir/sub", 2); });
    expect(EISDIR, [&] { s.rename("/b", "/dir"); });
    expect(ENOTDIR, [&] { s.rename("/dir", "/b"); });
    expect(EINVAL, [&] { s.rename("/b", "/c", 4); });
    healthy(s);
}
void test_concurrency() {
    Fixture f;
    Store s(f.directory, f.key);
    auto id = file(s, "/shared"), append = file(s, "/append");
    std::vector<std::thread> threads;
    std::mutex errors_mutex;
    std::vector<std::string> errors;
    for (int t = 0; t < 8; ++t)
        threads.emplace_back([&, t] {
            try {
                for (int i = 0; i < 20; ++i) {
                    write(s, id, Bytes(4096, static_cast<unsigned char>(t + 1)), t * 4096);
                    Bytes record(64, static_cast<unsigned char>(t + 1));
                    CHECK(s.write(append, record.data(), record.size(), 0, true) == record.size());
                    Bytes check(4096);
                    CHECK(s.read(id, check.data(), check.size(), t * 4096) == check.size());
                    CHECK(check == Bytes(4096, static_cast<unsigned char>(t + 1)));
                }
            } catch (const std::exception &e) {
                std::lock_guard guard(errors_mutex);
                errors.push_back(e.what());
            }
        });
    for (auto &thread : threads) {
        thread.join();
    }
    CHECK(errors.empty());
    CHECK(s.stat(append).st_size == 8 * 20 * 64);
    auto data = read(s, append);
    for (std::size_t i = 0; i < data.size(); i += 64)
        CHECK(std::all_of(data.begin() + static_cast<std::ptrdiff_t>(i),
                          data.begin() + static_cast<std::ptrdiff_t>(i + 64),
                          [&](auto c) { return c == data[i]; }));
    healthy(s);
}
void test_exclusive_and_key_checks() {
    Fixture f;
    {
        Store s(f.directory, f.key);
        expect(EBUSY, [&] { Store other(f.directory, f.key); });
    }
    auto wrong = f.root + "/wrong";
    create_key(wrong);
    expect(EACCES, [&] { Store s(f.directory, wrong); });
    CHECK(::chmod(f.key.c_str(), 0644) == 0);
    expect(EACCES, [&] { Store s(f.directory, f.key); });
    CHECK(::chmod(f.key.c_str(), 0600) == 0);
    CHECK(::symlink(f.key.c_str(), (f.root + "/link-key").c_str()) == 0);
    expect(ELOOP, [&] { Store s(f.directory, f.root + "/link-key"); });
    expect(EEXIST, [&] { Store::initialize(f.directory, f.key); });
}
void test_aead() {
    Key key{};
    std::fill(key.begin(), key.end(), 23);
    Bytes input{1, 2, 3, 4, 5};
    auto data = seal(key, "context", input);
    CHECK(unseal(key, "context", data) == input);
    CHECK(seal(key, "context", input) != data);
    expect(EIO, [&] { unseal(key, "other-context", data); });
    for (std::size_t i = 0; i < data.size(); ++i) {
        auto corrupt = data;
        corrupt[i] ^= 1;
        expect(EIO, [&] { unseal(key, "context", corrupt); });
    }
}
void test_corruption() {
    Fixture f;
    Id id;
    {
        Store s(f.directory, f.key);
        id = file(s, "/a");
        write(s, id, Bytes(4096, 12));
        s.sync();
    }
    sql(f.directory, "UPDATE chunks SET data=zeroblob(length(data))");
    {
        Store s(f.directory, f.key);
        expect(EIO, [&] { read(s, id); });
        CHECK(!s.check().empty());
        expect(EIO, [&] { s.gc(); });
    }
    Fixture missing;
    {
        Store s(missing.directory, missing.key);
        id = file(s, "/a");
        write(s, id, Bytes(4096, 12));
    }
    sql(missing.directory, "PRAGMA foreign_keys=OFF; DELETE FROM chunks");
    {
        Store s(missing.directory, missing.key);
        expect(EIO, [&] { read(s, id); });
        CHECK(!s.check().empty());
    }
    Fixture metadata;
    sql(metadata.directory, "UPDATE inodes SET mode=33188 WHERE id=1");
    {
        Store s(metadata.directory, metadata.key);
        CHECK(!s.check().empty());
    }
    Fixture mapping;
    {
        Store s(mapping.directory, mapping.key);
        id = file(s, "/a");
        write(s, id, Bytes(4096, 3));
    }
    sql(mapping.directory, "DELETE FROM refs");
    {
        Store s(mapping.directory, mapping.key);
        expect(EIO, [&] { read(s, id); });
        CHECK(!s.check().empty());
    }
    Fixture substituted;
    {
        Store s(substituted.directory, substituted.key);
        id = file(s, "/a");
        write(s, id, Bytes(4096, 3));
        auto other = file(s, "/b");
        write(s, other, Bytes(4096, 4));
    }
    sql(substituted.directory, "UPDATE refs SET chunk=(SELECT id FROM chunks ORDER BY id LIMIT 1)");
    {
        Store s(substituted.directory, substituted.key);
        CHECK(!s.check().empty());
    }
    Fixture config;
    sql(config.directory, "UPDATE config SET value='8192' WHERE name='chunk_size'");
    expect(EACCES, [&] { Store s(config.directory, config.key); });
}
void test_extreme_offsets() {
    Fixture f;
    Store s(f.directory, f.key);
    auto id = file(s, "/huge");
    write(s, id, Bytes{7}, INT64_MAX - 1);
    CHECK(s.stat(id).st_size == INT64_MAX);
    CHECK(s.seek(id, INT64_MAX - 1, SEEK_HOLE) == INT64_MAX);
    unsigned char c = 0;
    CHECK(s.read(id, &c, 1, INT64_MAX - 1) == 1 && c == 7);
    s.punch_hole(id, INT64_MAX - 1, 1);
    CHECK(s.stat(id).st_blocks == 0);
    healthy(s);
}
void test_rollback() {
    Fixture f;
    Store s(f.directory, f.key);
    auto id = file(s, "/a");
    write(s, id, Bytes(4096, 1));
    auto versions = s.history(id).size();
    s.set_commit_hook([](std::string_view point) {
        if (point == "before_commit")
            throw Error(ENOSPC, "Injected disk-full failure");
    });
    expect(ENOSPC, [&] { write(s, id, Bytes(8192, 2)); });
    s.set_commit_hook({});
    CHECK(read(s, id) == Bytes(4096, 1));
    CHECK(s.history(id).size() == versions);
    healthy(s);
}
void test_crash_write() {
    for (auto point : {"after_chunks", "before_commit", "after_commit"}) {
        Fixture f;
        Id id;
        {
            Store s(f.directory, f.key);
            id = file(s, "/a");
            write(s, id, Bytes(8192, 1));
        }
        auto pid = fork();
        CHECK(pid >= 0);
        if (pid == 0) {
            try {
                Store s(f.directory, f.key);
                s.set_commit_hook([point](std::string_view p) {
                    if (p == point)
                        ::kill(getpid(), SIGKILL);
                });
                write(s, id, Bytes(8192, 2));
                _exit(99);
            } catch (...) {
                _exit(98);
            }
        }
        int status = 0;
        CHECK(waitpid(pid, &status, 0) == pid);
        CHECK(WIFSIGNALED(status) && WTERMSIG(status) == SIGKILL);
        Store recovered(f.directory, f.key);
        CHECK(read(recovered, id) == Bytes(8192, std::string_view(point) == "after_commit" ? 2 : 1));
        healthy(recovered);
    }
}
void test_crash_rename() {
    for (auto point : {"before_commit", "after_commit"}) {
        Fixture f;
        Id a, b;
        {
            Store s(f.directory, f.key);
            a = file(s, "/a");
            b = file(s, "/b");
            write(s, a, Bytes{1});
            write(s, b, Bytes{2});
        }
        auto pid = fork();
        CHECK(pid >= 0);
        if (pid == 0) {
            try {
                Store s(f.directory, f.key);
                s.set_commit_hook([point](std::string_view p) {
                    if (p == point)
                        ::kill(getpid(), SIGKILL);
                });
                s.rename("/a", "/b");
                _exit(99);
            } catch (...) {
                _exit(98);
            }
        }
        int status = 0;
        CHECK(waitpid(pid, &status, 0) == pid);
        CHECK(WIFSIGNALED(status));
        Store recovered(f.directory, f.key);
        if (std::string_view(point) == "after_commit") {
            CHECK(recovered.lookup("/b") == a);
            expect(ENOENT, [&] { recovered.lookup("/a"); });
        } else {
            CHECK(recovered.lookup("/a") == a);
            CHECK(recovered.lookup("/b") == b);
        }
        healthy(recovered);
    }
}
void test_crash_maintenance() {
    for (bool collecting : {false, true})
        for (auto point : {"before_commit", "after_commit"}) {
            Fixture f;
            Id id;
            {
                Store s(f.directory, f.key);
                id = file(s, "/a");
                write(s, id, Bytes(4096, 1));
                write(s, id, Bytes(4096, 2));
                if (collecting)
                    s.prune(1);
            }
            auto pid = fork();
            CHECK(pid >= 0);
            if (pid == 0) {
                try {
                    Store s(f.directory, f.key);
                    s.set_commit_hook([point](std::string_view p) {
                        if (p == point)
                            ::kill(getpid(), SIGKILL);
                    });
                    if (collecting)
                        s.gc();
                    else
                        s.prune(1);
                    _exit(99);
                } catch (...) {
                    _exit(98);
                }
            }
            int status = 0;
            CHECK(waitpid(pid, &status, 0) == pid);
            CHECK(WIFSIGNALED(status) && WTERMSIG(status) == SIGKILL);
            Store recovered(f.directory, f.key);
            CHECK(read(recovered, id) == Bytes(4096, 2));
            healthy(recovered);
            const bool committed = std::string_view(point) == "after_commit";
            if (collecting)
                CHECK(recovered.stats().chunks == (committed ? 1 : 2));
            else
                CHECK(recovered.history(id).size() == (committed ? 1 : 3));
        }
}
} // namespace
int main(int argc, char **argv) {
    const std::vector<std::pair<const char *, std::function<void()>>> tests{
        {"namespace", test_namespace},
        {"chunk sizes", test_chunk_sizes},
        {"random I/O model", test_random_io_model},
        {"sparse files and hole punching", test_sparse_and_holes},
        {"deduplication compression cache", test_dedup_compression_cache},
        {"history garbage collection restore", test_history_gc_restore},
        {"permissions symlinks timestamps", test_permissions_symlinks_times},
        {"atomic rename", test_rename},
        {"multithreaded writes and append", test_concurrency},
        {"exclusive store and key security", test_exclusive_and_key_checks},
        {"authenticated encryption", test_aead},
        {"corruption detection", test_corruption},
        {"extreme sparse offsets", test_extreme_offsets},
        {"transaction rollback", test_rollback},
        {"SIGKILL write recovery", test_crash_write},
        {"SIGKILL rename recovery", test_crash_rename},
        {"SIGKILL pruning and GC recovery", test_crash_maintenance}};
    int failures = 0;
    int selected = -1;
    if (argc == 2)
        selected = std::stoi(argv[1]);
    else if (argc != 1)
        return 2;
    if (selected >= static_cast<int>(tests.size()) || selected < -1)
        return 2;
    int index = 0, executed = 0;
    for (auto &[name, test] : tests) {
        if (selected != -1 && index++ != selected)
            continue;
        ++executed;
        try {
            test();
            std::cout << "PASS " << name << std::endl;
        } catch (const std::exception &e) {
            ++failures;
            std::cerr << "FAIL " << name << ": " << e.what() << std::endl;
        }
    }
    std::cout << executed << " test groups, " << failures << " failures\n";
    return failures ? 1 : 0;
}
