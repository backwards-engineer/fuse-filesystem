#include "store.hpp"
#include "crypto.hpp"
#include "sql.hpp"
#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <fcntl.h>
#include <filesystem>
#include <limits>
#include <list>
#include <mutex>
#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <set>
#include <sys/file.h>
#include <unistd.h>
#include <unordered_map>
#include <zstd.h>

namespace localfs {
namespace {
Id now() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}
void ensure(bool condition, int error, const std::string &message) {
    if (!condition)
        throw Error(error, message);
}
Id scalar(sqlite3 *db, const char *sql) {
    Statement q(db, sql);
    ensure(q.row(), EIO, "Missing SQL result");
    return q.number(0);
}
void validate_chunk(std::size_t n) {
    ensure(n >= 4096 && n <= 4 * 1024 * 1024 && (n & (n - 1)) == 0, EINVAL,
           "Chunk size must be a power of two between 4096 and 4194304");
}
std::vector<std::string> components(std::string_view path) {
    ensure(!path.empty() && path[0] == '/' && path.find('\0') == path.npos, EINVAL,
           "An absolute filesystem path is required");
    std::vector<std::string> result;
    std::size_t start = 1;
    while (start < path.size()) {
        auto end = path.find('/', start);
        if (end == path.npos)
            end = path.size();
        auto part = path.substr(start, end - start);
        ensure(!part.empty() && part != "." && part != "..", EINVAL, "Noncanonical path");
        ensure(part.size() <= 255, ENAMETOOLONG, "Filename exceeds 255 bytes");
        result.emplace_back(part);
        start = end + 1;
    }
    return result;
}
struct Node {
    Id id{}, mode{}, uid{}, gid{}, size{}, atime{}, mtime{}, ctime{}, current{};
    std::string target;
};
} // namespace

struct Store::Impl {
    sqlite3 *db = nullptr;
    int lock_fd = -1;
    std::string directory;
    Key key{};
    std::size_t chunk_size = 0, cache_limit, cache_bytes = 0, hits = 0, misses = 0;
    int compression = 0;
    std::recursive_mutex mutex;
    std::function<void(std::string_view)> hook;
    struct Cached {
        Bytes data;
        std::list<std::string>::iterator position;
    };
    std::list<std::string> lru;
    std::unordered_map<std::string, Cached> cache;

    explicit Impl(std::size_t limit) : cache_limit(limit) {}
    ~Impl() {
        clear_cache();
        if (db)
            sqlite3_close_v2(db);
        if (lock_fd >= 0) {
            ::flock(lock_fd, LOCK_UN);
            ::close(lock_fd);
        }
        OPENSSL_cleanse(key.data(), key.size());
    }
    void clear_cache() {
        for (auto &[id, item] : cache) {
            (void)id;
            OPENSSL_cleanse(item.data.data(), item.data.size());
        }
        cache.clear();
        lru.clear();
        cache_bytes = 0;
    }
    void open(const std::string &path, const std::string &key_file, bool create) {
        directory = std::filesystem::absolute(path).string();
        struct stat st {};
        if (::lstat(directory.c_str(), &st) != 0)
            throw Error(errno, "Cannot inspect store directory");
        ensure(S_ISDIR(st.st_mode) && st.st_uid == ::geteuid() && !(st.st_mode & 077), EACCES,
               "Store must be a private directory owned by you (mode 0700)");
        lock_fd =
            ::open((directory + "/store.lock").c_str(), O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW, 0600);
        ensure(lock_fd >= 0, errno, "Cannot open store lock");
        ensure(::flock(lock_fd, LOCK_EX | LOCK_NB) == 0, EBUSY,
               "Store is already mounted or in use; unmount before administration");
        key = read_key(key_file);
        auto db_path = directory + "/store.db";
        if (create) {
            int fd = ::open(db_path.c_str(), O_CREAT | O_EXCL | O_WRONLY | O_CLOEXEC | O_NOFOLLOW, 0600);
            ensure(fd >= 0, errno, "Store already exists or cannot be created");
            ::close(fd);
        }
        sql_check(db, sqlite3_open_v2(db_path.c_str(), &db,
                                      SQLITE_OPEN_READWRITE | SQLITE_OPEN_FULLMUTEX | SQLITE_OPEN_NOFOLLOW,
                                      nullptr));
        sqlite3_busy_timeout(db, 5000);
        execute(db, "PRAGMA foreign_keys=ON; PRAGMA trusted_schema=OFF; PRAGMA synchronous=FULL;");
        if (create)
            execute(db, "PRAGMA journal_mode=WAL");
        Statement mode(db, "PRAGMA journal_mode");
        ensure(mode.row() && mode.text(0) == "wal", EIO, "Store requires SQLite WAL mode");
    }
    std::string config(const char *name) {
        Statement q(db, "SELECT value FROM config WHERE name=?");
        q.bind(1, std::string_view(name));
        ensure(q.row(), EIO, "Missing store configuration");
        return q.text(0);
    }
    void configure(const char *name, const std::string &value) {
        Statement q(db, "INSERT INTO config VALUES(?,?)");
        q.bind(1, std::string_view(name));
        q.bind(2, value);
        q.done();
    }
    Node node(Id inode) {
        Statement q(db,
                    "SELECT id,mode,uid,gid,size,atime,mtime,ctime,current,target FROM inodes WHERE id=?");
        q.bind(1, inode);
        ensure(q.row(), ENOENT, "Inode not found");
        return {q.number(0), q.number(1), q.number(2), q.number(3), q.number(4),
                q.number(5), q.number(6), q.number(7), q.number(8), q.text(9)};
    }
    Id child(Id parent, std::string_view name) {
        ensure(S_ISDIR(node(parent).mode), ENOTDIR, "Parent is not a directory");
        Statement q(db, "SELECT child FROM entries WHERE parent=? AND name=?");
        q.bind(1, parent);
        q.bind(2, name);
        return q.row() ? q.number(0) : 0;
    }
    Id lookup(std::string_view path) {
        Id inode = 1;
        for (auto &part : components(path)) {
            inode = child(inode, part);
            ensure(inode != 0, ENOENT, "Path not found");
        }
        return inode;
    }
    Id parent_of(Id inode) {
        if (inode == 1)
            return 1;
        Statement q(db, "SELECT parent FROM entries WHERE child=?");
        q.bind(1, inode);
        ensure(q.row(), ENOENT, "Inode has been unlinked");
        return q.number(0);
    }
    std::string path_at(Id parent_id, std::string_view name) {
        ensure(!name.empty() && name != "." && name != ".." && name.find('/') == name.npos &&
                   name.find('\0') == name.npos,
               EINVAL, "Invalid entry name");
        ensure(S_ISDIR(node(parent_id).mode), ENOTDIR, "Parent is not a directory");
        std::string path = "/" + std::string(name);
        std::set<Id> visited;
        while (parent_id != 1) {
            ensure(visited.insert(parent_id).second, EIO, "Directory cycle");
            Statement q(db, "SELECT parent,name FROM entries WHERE child=?");
            q.bind(1, parent_id);
            ensure(q.row(), ENOENT, "Parent directory has been unlinked");
            path = "/" + q.text(1) + path;
            parent_id = q.number(0);
        }
        return path;
    }
    std::pair<Id, std::string> parent(std::string_view path) {
        auto parts = components(path);
        ensure(!parts.empty(), EBUSY, "Cannot modify root");
        Id inode = 1;
        for (std::size_t i = 0; i + 1 < parts.size(); ++i) {
            inode = child(inode, parts[i]);
            ensure(inode != 0, ENOENT, "Parent not found");
        }
        ensure(S_ISDIR(node(inode).mode), ENOTDIR, "Parent is not a directory");
        return {inode, parts.back()};
    }
    void touch(Id inode) {
        Statement q(db, "UPDATE inodes SET mtime=?,ctime=? WHERE id=?");
        auto time = now();
        q.bind(1, time);
        q.bind(2, time);
        q.bind(3, inode);
        q.done();
    }
    void changed(Id inode) {
        Statement q(db, "UPDATE inodes SET ctime=? WHERE id=?");
        q.bind(1, now());
        q.bind(2, inode);
        q.done();
    }
    void entry(Id parent, std::string_view name, Id child_id) {
        Statement q(db, "INSERT INTO entries VALUES(?,?,?)");
        q.bind(1, parent);
        q.bind(2, name);
        q.bind(3, child_id);
        q.done();
    }
    void delete_entry(Id parent, std::string_view name) {
        Statement q(db, "DELETE FROM entries WHERE parent=? AND name=?");
        q.bind(1, parent);
        q.bind(2, name);
        q.done();
    }
    bool empty(Id inode) {
        Statement q(db, "SELECT 1 FROM entries WHERE parent=? LIMIT 1");
        q.bind(1, inode);
        return !q.row();
    }
    void no_cycle(Id inode, Id parent_id) {
        std::set<Id> visited;
        while (true) {
            ensure(inode != parent_id, EINVAL, "Cannot move a directory into itself");
            if (parent_id == 1)
                break;
            ensure(visited.insert(parent_id).second, EIO, "Directory cycle detected");
            Statement q(db, "SELECT parent FROM entries WHERE child=?");
            q.bind(1, parent_id);
            ensure(q.row(), EIO, "Disconnected directory");
            parent_id = q.number(0);
        }
    }
    std::string aad(const std::string &id, int codec, Id raw) {
        return "local-fusefs:1:" + id + ":" + std::to_string(codec) + ":" + std::to_string(raw);
    }
    void cache_put(const std::string &id, const Bytes &bytes) {
        if (bytes.size() > cache_limit || cache.contains(id))
            return;
        while (cache_bytes + bytes.size() > cache_limit && !lru.empty()) {
            auto it = cache.find(lru.back());
            cache_bytes -= it->second.data.size();
            OPENSSL_cleanse(it->second.data.data(), it->second.data.size());
            cache.erase(it);
            lru.pop_back();
        }
        lru.push_front(id);
        cache.emplace(id, Cached{bytes, lru.begin()});
        cache_bytes += bytes.size();
    }
    Bytes load(const std::string &id, bool bypass_cache = false) {
        auto cached = cache.find(id);
        if (!bypass_cache && cached != cache.end()) {
            ++hits;
            lru.splice(lru.begin(), lru, cached->second.position);
            return cached->second.data;
        }
        ++misses;
        Statement q(db, "SELECT codec,raw_size,data FROM chunks WHERE id=?");
        q.bind(1, id);
        ensure(q.row(), EIO, "Missing content chunk " + id);
        auto codec = static_cast<int>(q.number(0));
        auto raw = q.number(1);
        ensure(raw == static_cast<Id>(chunk_size) && (codec == 0 || codec == 1), EIO,
               "Invalid chunk metadata");
        auto encoded = unseal(key, aad(id, codec, raw), q.bytes(2));
        Bytes plain;
        if (codec == 1) {
            plain.resize(chunk_size);
            auto result = ZSTD_decompress(plain.data(), plain.size(), encoded.data(), encoded.size());
            ensure(!ZSTD_isError(result) && result == chunk_size, EIO, "Compressed chunk is corrupt");
        } else {
            plain = std::move(encoded);
        }
        ensure(plain.size() == chunk_size && digest(plain) == id, EIO, "Content hash mismatch");
        if (!bypass_cache)
            cache_put(id, plain);
        return plain;
    }
    Bytes block(Id version, Id index) {
        Statement q(db, "SELECT chunk FROM refs WHERE version=? AND idx=?");
        q.bind(1, version);
        q.bind(2, index);
        return q.row() ? load(q.text(0)) : Bytes(chunk_size, 0);
    }
    void put(Id version, Id index, const Bytes &bytes) {
        ensure(bytes.size() == chunk_size, EIO, "Invalid block size");
        if (std::all_of(bytes.begin(), bytes.end(), [](unsigned char c) { return c == 0; })) {
            Statement q(db, "DELETE FROM refs WHERE version=? AND idx=?");
            q.bind(1, version);
            q.bind(2, index);
            q.done();
            return;
        }
        auto id = digest(bytes);
        Statement exists(db, "SELECT 1 FROM chunks WHERE id=?");
        exists.bind(1, id);
        if (exists.row()) {
            ensure(load(id) == bytes, EIO, "Content address collision or corruption");
        } else {
            int codec = 0;
            Bytes encoded = bytes;
            if (compression != 0) {
                Bytes compressed(ZSTD_compressBound(bytes.size()));
                auto length = ZSTD_compress(compressed.data(), compressed.size(), bytes.data(), bytes.size(),
                                            compression);
                ensure(!ZSTD_isError(length), EIO, "Compression failed");
                if (length < bytes.size()) {
                    compressed.resize(length);
                    encoded = std::move(compressed);
                    codec = 1;
                }
            }
            auto encrypted = seal(key, aad(id, codec, static_cast<Id>(bytes.size())), encoded);
            Statement q(db, "INSERT INTO chunks VALUES(?,?,?,?)");
            q.bind(1, id);
            q.bind(2, codec);
            q.bind(3, static_cast<Id>(bytes.size()));
            q.bind(4, encrypted);
            q.done();
        }
        Statement q(
            db, "INSERT INTO refs VALUES(?,?,?) ON CONFLICT(version,idx) DO UPDATE SET chunk=excluded.chunk");
        q.bind(1, version);
        q.bind(2, index);
        q.bind(3, id);
        q.done();
    }
    Id version(Id inode, Id size, Id previous) {
        if (previous)
            verify_version(previous);
        Statement q(db, "INSERT INTO versions(inode,size,created) VALUES(?,?,?)");
        q.bind(1, inode);
        q.bind(2, size);
        q.bind(3, now());
        q.done();
        Id id = sqlite3_last_insert_rowid(db);
        if (previous) {
            Statement copy(db, "INSERT INTO refs SELECT ?,idx,chunk FROM refs WHERE version=?");
            copy.bind(1, id);
            copy.bind(2, previous);
            copy.done();
        }
        return id;
    }
    std::string version_mac(Id version_id) {
        std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)> context(EVP_MD_CTX_new(), EVP_MD_CTX_free);
        ensure(context && EVP_DigestInit_ex(context.get(), EVP_sha256(), nullptr) == 1, EIO,
               "Manifest hash initialization failed");
        auto append = [&](const std::string &text) {
            ensure(EVP_DigestUpdate(context.get(), text.data(), text.size()) == 1, EIO,
                   "Manifest hashing failed");
        };
        Statement version_row(db, "SELECT inode,size,created FROM versions WHERE id=?");
        version_row.bind(1, version_id);
        ensure(version_row.row(), EIO, "Missing version manifest");
        append(std::to_string(version_id) + ":" + std::to_string(version_row.number(0)) + ":" +
               std::to_string(version_row.number(1)) + ":" + std::to_string(version_row.number(2)) + ";");
        Statement refs(db, "SELECT idx,chunk FROM refs WHERE version=? ORDER BY idx");
        refs.bind(1, version_id);
        while (refs.row())
            append(std::to_string(refs.number(0)) + ":" + refs.text(1) + ";");
        std::array<unsigned char, 32> hash{};
        unsigned length = 0;
        ensure(EVP_DigestFinal_ex(context.get(), hash.data(), &length) == 1 && length == hash.size(), EIO,
               "Manifest hash finalization failed");
        return key_check(key, std::string("local-fusefs-manifest:1:") +
                                  std::string(reinterpret_cast<const char *>(hash.data()), hash.size()));
    }
    void verify_version(Id version_id) {
        Statement q(db, "SELECT mac FROM versions WHERE id=?");
        q.bind(1, version_id);
        ensure(q.row(), EIO, "Missing version");
        auto stored = q.text(0), expected = version_mac(version_id);
        ensure(stored.size() == expected.size() &&
                   CRYPTO_memcmp(stored.data(), expected.data(), expected.size()) == 0,
               EIO, "Version manifest authentication failed");
    }
    void current(Id inode, Id version_id, Id size) {
        Statement sign(db, "UPDATE versions SET mac=? WHERE id=?");
        sign.bind(1, version_mac(version_id));
        sign.bind(2, version_id);
        sign.done();
        Statement q(db, "UPDATE inodes SET current=?,size=?,mtime=?,ctime=?,mode=mode & ~3072 WHERE id=?");
        auto time = now();
        q.bind(1, version_id);
        q.bind(2, size);
        q.bind(3, time);
        q.bind(4, time);
        q.bind(5, inode);
        q.done();
    }
    Node regular(Id inode) {
        auto n = node(inode);
        ensure(S_ISREG(n.mode), S_ISDIR(n.mode) ? EISDIR : EINVAL, "Not a regular file");
        Statement q(db, "SELECT inode,size FROM versions WHERE id=?");
        q.bind(1, n.current);
        ensure(q.row() && q.number(0) == inode && q.number(1) == n.size, EIO,
               "Current version metadata mismatch");
        return n;
    }
};

void Store::initialize(const std::string &directory, const std::string &key_file, std::size_t chunk_size,
                       int compression) {
    validate_chunk(chunk_size);
    ensure(compression >= 0 && compression <= 19, EINVAL,
           "Compression must be 0 (off) or Zstandard level 1..19");
    if (::mkdir(directory.c_str(), 0700) != 0 && errno != EEXIST)
        throw Error(errno, "Cannot create store directory");
    auto dir = std::filesystem::weakly_canonical(directory);
    auto key_path = std::filesystem::weakly_canonical(key_file);
    auto relative = key_path.lexically_relative(dir);
    ensure(relative.empty() || *relative.begin() == "..", EINVAL,
           "Keep the key file outside the storage directory");
    if (::access(key_file.c_str(), F_OK) != 0) {
        ensure(errno == ENOENT, errno, "Cannot inspect key path");
        create_key(key_file);
    }
    Impl p(0);
    p.open(directory, key_file, true);
    Transaction tx(p.db, p.hook);
    execute(p.db, R"SQL(
      CREATE TABLE config(name TEXT PRIMARY KEY, value TEXT NOT NULL);
      CREATE TABLE inodes(id INTEGER PRIMARY KEY AUTOINCREMENT, mode INTEGER NOT NULL, uid INTEGER NOT NULL, gid INTEGER NOT NULL,
        size INTEGER NOT NULL CHECK(size>=0), atime INTEGER NOT NULL, mtime INTEGER NOT NULL, ctime INTEGER NOT NULL,
        current INTEGER NOT NULL DEFAULT 0, target TEXT NOT NULL DEFAULT '');
      CREATE TABLE entries(parent INTEGER NOT NULL REFERENCES inodes(id), name TEXT NOT NULL CHECK(length(name)>0),
        child INTEGER NOT NULL UNIQUE REFERENCES inodes(id), PRIMARY KEY(parent,name));
      CREATE TABLE versions(id INTEGER PRIMARY KEY AUTOINCREMENT, inode INTEGER NOT NULL REFERENCES inodes(id) ON DELETE CASCADE,
        size INTEGER NOT NULL CHECK(size>=0), created INTEGER NOT NULL, mac TEXT NOT NULL DEFAULT '');
      CREATE INDEX version_inode ON versions(inode,id);
      CREATE TABLE chunks(id TEXT PRIMARY KEY CHECK(length(id)=64), codec INTEGER NOT NULL CHECK(codec IN (0,1)),
        raw_size INTEGER NOT NULL CHECK(raw_size>0), data BLOB NOT NULL);
      CREATE TABLE refs(version INTEGER NOT NULL REFERENCES versions(id) ON DELETE CASCADE, idx INTEGER NOT NULL CHECK(idx>=0),
        chunk TEXT NOT NULL REFERENCES chunks(id), PRIMARY KEY(version,idx));
      CREATE INDEX refs_chunk ON refs(chunk);
    )SQL");
    p.configure("format", "1");
    p.configure("chunk_size", std::to_string(chunk_size));
    p.configure("compression", std::to_string(compression));
    p.configure("key_check", key_check(p.key, "local-fusefs:1:" + std::to_string(chunk_size) + ":" +
                                                  std::to_string(compression)));
    Statement root(p.db,
                   "INSERT INTO inodes(id,mode,uid,gid,size,atime,mtime,ctime) VALUES(1,?,?,?,?,?,?,?)");
    root.bind(1, S_IFDIR | 0755);
    root.bind(2, ::getuid());
    root.bind(3, ::getgid());
    root.bind(4, 0);
    auto time = now();
    root.bind(5, time);
    root.bind(6, time);
    root.bind(7, time);
    root.done();
    tx.commit();
    int fd = ::open(directory.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    ensure(fd >= 0, errno, "Cannot open store directory");
    int rc = ::fsync(fd);
    ::close(fd);
    ensure(rc == 0, errno, "Cannot persist store directory");
    auto parent = std::filesystem::absolute(directory).parent_path();
    fd = ::open(parent.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    ensure(fd >= 0, errno, "Cannot open parent directory");
    rc = ::fsync(fd);
    ::close(fd);
    ensure(rc == 0, errno, "Cannot persist parent directory");
}
Store::Store(const std::string &directory, const std::string &key_file, std::size_t cache_limit)
    : impl_(std::make_unique<Impl>(cache_limit)) {
    auto &p = *impl_;
    p.open(directory, key_file, false);
    ensure(p.config("format") == "1", EINVAL, "Unsupported storage format");
    p.chunk_size = std::stoull(p.config("chunk_size"));
    validate_chunk(p.chunk_size);
    p.compression = std::stoi(p.config("compression"));
    ensure(p.compression >= 0 && p.compression <= 19, EIO, "Invalid compression level");
    auto expected = key_check(p.key, "local-fusefs:1:" + std::to_string(p.chunk_size) + ":" +
                                         std::to_string(p.compression));
    auto actual = p.config("key_check");
    ensure(actual.size() == expected.size() &&
               CRYPTO_memcmp(actual.data(), expected.data(), expected.size()) == 0,
           EACCES, "Wrong key or modified store configuration");
}
Store::~Store() = default;
Id Store::lookup(std::string_view path) {
    auto &p = *impl_;
    std::lock_guard guard(p.mutex);
    return p.lookup(path);
}
Id Store::lookup_at(Id parent, std::string_view name) {
    auto &p = *impl_;
    std::lock_guard guard(p.mutex);
    if (name == ".") {
        p.node(parent);
        return parent;
    }
    if (name == "..")
        return p.parent_of(parent);
    auto id = p.child(parent, name);
    ensure(id, ENOENT, "Entry not found");
    return id;
}
Id Store::create_at(Id parent, std::string_view name, mode_t mode, uid_t uid, gid_t gid,
                    std::string_view target) {
    auto &p = *impl_;
    std::lock_guard guard(p.mutex);
    return create(p.path_at(parent, name), mode, uid, gid, target);
}
void Store::remove_at(Id parent, std::string_view name, bool directory) {
    auto &p = *impl_;
    std::lock_guard guard(p.mutex);
    remove(p.path_at(parent, name), directory);
}
void Store::rename_at(Id parent, std::string_view name, Id new_parent, std::string_view new_name,
                      unsigned flags) {
    auto &p = *impl_;
    std::lock_guard guard(p.mutex);
    rename(p.path_at(parent, name), p.path_at(new_parent, new_name), flags);
}
struct stat Store::stat(Id inode) {
    auto &p = *impl_;
    std::lock_guard guard(p.mutex);
    auto n = p.node(inode);
    struct stat result {};
    result.st_ino = static_cast<ino_t>(n.id);
    result.st_mode = static_cast<mode_t>(n.mode);
    result.st_uid = static_cast<uid_t>(n.uid);
    result.st_gid = static_cast<gid_t>(n.gid);
    result.st_size = n.size;
    result.st_blksize = static_cast<blksize_t>(p.chunk_size);
    result.st_atim = {n.atime / 1000000000, n.atime % 1000000000};
    result.st_mtim = {n.mtime / 1000000000, n.mtime % 1000000000};
    result.st_ctim = {n.ctime / 1000000000, n.ctime % 1000000000};
    Statement links(p.db, "SELECT COUNT(*) FROM entries WHERE child=?");
    links.bind(1, inode);
    links.row();
    result.st_nlink = static_cast<nlink_t>(links.number(0));
    if (S_ISDIR(n.mode)) {
        Statement dirs(p.db, "SELECT COUNT(*) FROM entries e JOIN inodes i ON e.child=i.id WHERE e.parent=? "
                             "AND (i.mode & 61440)=16384");
        dirs.bind(1, inode);
        dirs.row();
        result.st_nlink = (inode == 1 || result.st_nlink != 0) ? static_cast<nlink_t>(2 + dirs.number(0)) : 0;
    }
    if (S_ISREG(n.mode)) {
        Statement refs(p.db, "SELECT COUNT(*) FROM refs WHERE version=?");
        refs.bind(1, n.current);
        refs.row();
        result.st_blocks = refs.number(0) * static_cast<Id>(p.chunk_size / 512);
    }
    return result;
}
std::vector<Entry> Store::list(Id inode) {
    auto &p = *impl_;
    std::lock_guard guard(p.mutex);
    ensure(S_ISDIR(p.node(inode).mode), ENOTDIR, "Not a directory");
    Statement q(p.db, "SELECT name,child FROM entries WHERE parent=? ORDER BY name COLLATE BINARY");
    q.bind(1, inode);
    std::vector<Entry> result;
    while (q.row())
        result.push_back({q.text(0), q.number(1)});
    return result;
}
Id Store::create(std::string_view path, mode_t mode, uid_t uid, gid_t gid, std::string_view target) {
    auto &p = *impl_;
    std::lock_guard guard(p.mutex);
    ensure(S_ISDIR(mode) || S_ISREG(mode) || S_ISLNK(mode), EOPNOTSUPP, "Unsupported file type");
    ensure(target.size() <= 4095 && target.find('\0') == target.npos, ENAMETOOLONG, "Invalid symlink target");
    auto [parent, name] = p.parent(path);
    ensure(!p.child(parent, name), EEXIST, "Path exists");
    auto parent_node = p.node(parent);
    if (parent_node.mode & S_ISGID) {
        gid = static_cast<gid_t>(parent_node.gid);
        if (S_ISDIR(mode))
            mode |= S_ISGID;
    }
    Transaction tx(p.db, p.hook);
    Statement q(p.db,
                "INSERT INTO inodes(mode,uid,gid,size,atime,mtime,ctime,target) VALUES(?,?,?,?,?,?,?,?)");
    q.bind(1, mode);
    q.bind(2, uid);
    q.bind(3, gid);
    q.bind(4, S_ISLNK(mode) ? static_cast<Id>(target.size()) : 0);
    auto time = now();
    q.bind(5, time);
    q.bind(6, time);
    q.bind(7, time);
    q.bind(8, target.empty() ? std::string_view("") : target);
    q.done();
    auto inode = sqlite3_last_insert_rowid(p.db);
    p.entry(parent, name, inode);
    if (S_ISREG(mode))
        p.current(inode, p.version(inode, 0, 0), 0);
    p.touch(parent);
    tx.commit();
    return inode;
}
void Store::remove(std::string_view path, bool directory) {
    auto &p = *impl_;
    std::lock_guard guard(p.mutex);
    auto [parent, name] = p.parent(path);
    auto inode = p.child(parent, name);
    ensure(inode, ENOENT, "Path not found");
    auto n = p.node(inode);
    ensure(directory ? S_ISDIR(n.mode) : !S_ISDIR(n.mode), directory ? ENOTDIR : EISDIR, "Wrong file type");
    if (directory)
        ensure(p.empty(inode), ENOTEMPTY, "Directory is not empty");
    Transaction tx(p.db, p.hook);
    p.delete_entry(parent, name);
    p.touch(parent);
    p.changed(inode);
    tx.commit();
}
void Store::rename(std::string_view from, std::string_view to, unsigned flags) {
    auto &p = *impl_;
    std::lock_guard guard(p.mutex);
    ensure(flags <= 2, EINVAL, "Unsupported rename flags");
    auto [src_parent, src_name] = p.parent(from);
    auto [dst_parent, dst_name] = p.parent(to);
    auto source = p.child(src_parent, src_name);
    auto dest = p.child(dst_parent, dst_name);
    ensure(source, ENOENT, "Source not found");
    if (flags == 1)
        ensure(!dest, EEXIST, "Destination exists");
    if (flags == 2)
        ensure(dest, ENOENT, "Exchange destination not found");
    if (source == dest)
        return;
    auto src = p.node(source);
    if (S_ISDIR(src.mode))
        p.no_cycle(source, dst_parent);
    if (dest) {
        auto dst = p.node(dest);
        if (flags == 2) {
            if (S_ISDIR(dst.mode))
                p.no_cycle(dest, src_parent);
        } else {
            ensure(S_ISDIR(src.mode) == S_ISDIR(dst.mode), S_ISDIR(src.mode) ? ENOTDIR : EISDIR,
                   "Rename type mismatch");
            if (S_ISDIR(dst.mode))
                ensure(p.empty(dest), ENOTEMPTY, "Destination directory is not empty");
        }
    }
    Transaction tx(p.db, p.hook);
    p.delete_entry(src_parent, src_name);
    if (dest)
        p.delete_entry(dst_parent, dst_name);
    p.entry(dst_parent, dst_name, source);
    if (flags == 2)
        p.entry(src_parent, src_name, dest);
    p.changed(source);
    if (dest)
        p.changed(dest);
    p.touch(src_parent);
    p.touch(dst_parent);
    tx.commit();
}
std::string Store::readlink(Id inode) {
    auto &p = *impl_;
    std::lock_guard guard(p.mutex);
    auto n = p.node(inode);
    ensure(S_ISLNK(n.mode), EINVAL, "Not a symlink");
    return n.target;
}
std::size_t Store::read(Id inode, void *buffer, std::size_t size, Id offset, Id version_id) {
    auto &p = *impl_;
    std::lock_guard guard(p.mutex);
    auto n = p.regular(inode);
    ensure(offset >= 0, EINVAL, "Negative read offset");
    if (version_id) {
        Statement q(p.db, "SELECT size FROM versions WHERE id=? AND inode=?");
        q.bind(1, version_id);
        q.bind(2, inode);
        ensure(q.row(), ENOENT, "Version not found");
        n.size = q.number(0);
    } else
        version_id = n.current;
    p.verify_version(version_id);
    if (offset >= n.size)
        return 0;
    auto count = std::min(size, static_cast<std::size_t>(n.size - offset));
    auto *output = static_cast<unsigned char *>(buffer);
    for (std::size_t done = 0; done < count;) {
        auto pos = offset + static_cast<Id>(done);
        auto index = pos / static_cast<Id>(p.chunk_size);
        auto within = static_cast<std::size_t>(pos % static_cast<Id>(p.chunk_size));
        auto amount = std::min(count - done, p.chunk_size - within);
        auto bytes = p.block(version_id, index);
        std::memcpy(output + done, bytes.data() + within, amount);
        done += amount;
    }
    return count;
}
std::size_t Store::write(Id inode, const void *buffer, std::size_t size, Id offset, bool append) {
    auto &p = *impl_;
    std::lock_guard guard(p.mutex);
    auto n = p.regular(inode);
    if (append)
        offset = n.size;
    ensure(offset >= 0 && size <= static_cast<std::uint64_t>(std::numeric_limits<Id>::max() - offset), EFBIG,
           "Write offset overflow");
    if (!size)
        return 0;
    auto length = std::max(n.size, offset + static_cast<Id>(size));
    Transaction tx(p.db, p.hook);
    auto version_id = p.version(inode, length, n.current);
    const auto *input = static_cast<const unsigned char *>(buffer);
    for (std::size_t done = 0; done < size;) {
        auto pos = offset + static_cast<Id>(done);
        auto index = pos / static_cast<Id>(p.chunk_size);
        auto within = static_cast<std::size_t>(pos % static_cast<Id>(p.chunk_size));
        auto amount = std::min(size - done, p.chunk_size - within);
        auto bytes =
            (within == 0 && amount == p.chunk_size) ? Bytes(p.chunk_size) : p.block(version_id, index);
        std::memcpy(bytes.data() + within, input + done, amount);
        p.put(version_id, index, bytes);
        done += amount;
    }
    if (p.hook)
        p.hook("after_chunks");
    p.current(inode, version_id, length);
    tx.commit();
    return size;
}
void Store::truncate(Id inode, Id size) {
    auto &p = *impl_;
    std::lock_guard guard(p.mutex);
    auto n = p.regular(inode);
    ensure(size >= 0, EINVAL, "Negative size");
    if (size == n.size)
        return;
    Transaction tx(p.db, p.hook);
    auto version_id = p.version(inode, size, n.current);
    if (size < n.size) {
        auto index = size / static_cast<Id>(p.chunk_size);
        auto within = static_cast<std::size_t>(size % static_cast<Id>(p.chunk_size));
        Statement q(p.db, "DELETE FROM refs WHERE version=? AND idx>=?");
        q.bind(1, version_id);
        q.bind(2, index + (within != 0));
        q.done();
        if (within) {
            auto bytes = p.block(version_id, index);
            std::fill(bytes.begin() + static_cast<std::ptrdiff_t>(within), bytes.end(), 0);
            p.put(version_id, index, bytes);
        }
    }
    p.current(inode, version_id, size);
    tx.commit();
}
void Store::chmod(Id inode, mode_t mode) {
    auto &p = *impl_;
    std::lock_guard guard(p.mutex);
    auto n = p.node(inode);
    Transaction tx(p.db, p.hook);
    Statement q(p.db, "UPDATE inodes SET mode=?,ctime=? WHERE id=?");
    q.bind(1, (n.mode & S_IFMT) | (mode & 07777));
    q.bind(2, now());
    q.bind(3, inode);
    q.done();
    tx.commit();
}
void Store::chown(Id inode, uid_t uid, gid_t gid) {
    auto &p = *impl_;
    std::lock_guard guard(p.mutex);
    auto n = p.node(inode);
    Transaction tx(p.db, p.hook);
    Statement q(p.db, "UPDATE inodes SET uid=?,gid=?,mode=mode & ~3072,ctime=? WHERE id=?");
    q.bind(1, uid == static_cast<uid_t>(-1) ? n.uid : uid);
    q.bind(2, gid == static_cast<gid_t>(-1) ? n.gid : gid);
    q.bind(3, now());
    q.bind(4, inode);
    q.done();
    tx.commit();
}
void Store::utimens(Id inode, const struct timespec times[2]) {
    auto &p = *impl_;
    std::lock_guard guard(p.mutex);
    auto n = p.node(inode);
    auto time = now();
    auto convert = [time](timespec t, Id previous) {
        if (t.tv_nsec == UTIME_OMIT)
            return previous;
        if (t.tv_nsec == UTIME_NOW)
            return time;
        ensure(t.tv_nsec >= 0 && t.tv_nsec < 1000000000 && t.tv_sec >= 0 &&
                   t.tv_sec <= (std::numeric_limits<Id>::max() - t.tv_nsec) / 1000000000,
               EINVAL, "Invalid timestamp");
        return static_cast<Id>(t.tv_sec) * 1000000000 + t.tv_nsec;
    };
    Transaction tx(p.db, p.hook);
    Statement q(p.db, "UPDATE inodes SET atime=?,mtime=?,ctime=? WHERE id=?");
    q.bind(1, times ? convert(times[0], n.atime) : time);
    q.bind(2, times ? convert(times[1], n.mtime) : time);
    q.bind(3, time);
    q.bind(4, inode);
    q.done();
    tx.commit();
}
Id Store::seek(Id inode, Id offset, int whence) {
    auto &p = *impl_;
    std::lock_guard guard(p.mutex);
    auto n = p.regular(inode);
    ensure(offset >= 0, EINVAL, "Negative seek");
    ensure(offset < n.size, ENXIO, "Seek beyond EOF");
    ensure(whence == SEEK_DATA || whence == SEEK_HOLE, EINVAL, "Unsupported seek");
    auto index = offset / static_cast<Id>(p.chunk_size);
    Statement q(p.db, "SELECT idx FROM refs WHERE version=? AND idx>=? ORDER BY idx");
    q.bind(1, n.current);
    q.bind(2, index);
    if (whence == SEEK_DATA) {
        ensure(q.row(), ENXIO, "No data after offset");
        return std::max(offset, q.number(0) * static_cast<Id>(p.chunk_size));
    }
    Id next = index;
    while (q.row()) {
        if (q.number(0) != next)
            break;
        ++next;
    }
    if (next == index)
        return offset;
    if (next > (n.size - 1) / static_cast<Id>(p.chunk_size))
        return n.size;
    return next * static_cast<Id>(p.chunk_size);
}
void Store::punch_hole(Id inode, Id offset, Id length) {
    auto &p = *impl_;
    std::lock_guard guard(p.mutex);
    auto n = p.regular(inode);
    ensure(offset >= 0 && length > 0 && length <= std::numeric_limits<Id>::max() - offset, EINVAL,
           "Invalid hole range");
    if (offset >= n.size)
        return;
    auto end = std::min(n.size, offset + length);
    auto first = offset / static_cast<Id>(p.chunk_size);
    auto last = (end - 1) / static_cast<Id>(p.chunk_size);
    Transaction tx(p.db, p.hook);
    auto version_id = p.version(inode, n.size, n.current);
    // Visit allocated chunks only, even for multi-terabyte holes.
    std::vector<Id> indices;
    {
        Statement q(p.db, "SELECT idx FROM refs WHERE version=? AND idx BETWEEN ? AND ?");
        q.bind(1, version_id);
        q.bind(2, first);
        q.bind(3, last);
        while (q.row())
            indices.push_back(q.number(0));
    }
    for (auto index : indices) {
        auto bytes = p.block(version_id, index);
        auto base = index * static_cast<Id>(p.chunk_size);
        auto begin = static_cast<std::size_t>(std::max(offset, base) - base);
        auto finish = static_cast<std::size_t>(std::min(end - base, static_cast<Id>(p.chunk_size)));
        std::fill(bytes.begin() + static_cast<std::ptrdiff_t>(begin),
                  bytes.begin() + static_cast<std::ptrdiff_t>(finish), 0);
        p.put(version_id, index, bytes);
    }
    p.current(inode, version_id, n.size);
    tx.commit();
}
void Store::sync() {
    auto &p = *impl_;
    std::lock_guard guard(p.mutex);
    sql_check(p.db, sqlite3_wal_checkpoint_v2(p.db, nullptr, SQLITE_CHECKPOINT_FULL, nullptr, nullptr));
}
struct statvfs Store::space() {
    auto &p = *impl_;
    std::lock_guard guard(p.mutex);
    struct statvfs result {};
    if (::statvfs(p.directory.c_str(), &result) != 0)
        throw Error(errno, "Cannot inspect backing space");
    result.f_namemax = 255;
    return result;
}
std::vector<Version> Store::history(Id inode) {
    auto &p = *impl_;
    std::lock_guard guard(p.mutex);
    p.regular(inode);
    Statement q(p.db, "SELECT id,size,created FROM versions WHERE inode=? ORDER BY id DESC");
    q.bind(1, inode);
    std::vector<Version> result;
    while (q.row())
        result.push_back({q.number(0), q.number(1), q.number(2)});
    return result;
}
void Store::restore(Id inode, Id version_id) {
    auto &p = *impl_;
    std::lock_guard guard(p.mutex);
    p.regular(inode);
    Statement q(p.db, "SELECT size FROM versions WHERE id=? AND inode=?");
    q.bind(1, version_id);
    q.bind(2, inode);
    ensure(q.row(), ENOENT, "Version not found");
    auto size = q.number(0);
    p.verify_version(version_id);
    // Validate every referenced chunk before making an old version current.
    {
        Statement refs(p.db, "SELECT chunk FROM refs WHERE version=?");
        refs.bind(1, version_id);
        while (refs.row())
            p.load(refs.text(0), true);
    }
    Transaction tx(p.db, p.hook);
    auto replacement = p.version(inode, size, version_id);
    p.current(inode, replacement, size);
    tx.commit();
}
Id Store::prune(std::size_t keep) {
    auto &p = *impl_;
    std::lock_guard guard(p.mutex);
    ensure(keep >= 1 && keep <= 1000000, EINVAL, "Keep count must be 1..1000000");
    Transaction tx(p.db, p.hook);
    Statement q(p.db, R"SQL(DELETE FROM versions WHERE id IN (
      SELECT id FROM (SELECT v.id, ROW_NUMBER() OVER(PARTITION BY inode ORDER BY v.id DESC) AS rank FROM versions v)
      WHERE rank>?) AND id NOT IN (SELECT current FROM inodes))SQL");
    q.bind(1, static_cast<Id>(keep));
    q.done();
    auto count = sqlite3_changes(p.db);
    tx.commit();
    return count;
}
Id Store::gc(bool purge_deleted) {
    auto &p = *impl_;
    std::lock_guard guard(p.mutex);
    ensure(check().empty(), EIO, "GC refused: run fsck and recover the corrupt store first");
    Transaction tx(p.db, p.hook);
    if (purge_deleted)
        execute(p.db, "DELETE FROM inodes WHERE id<>1 AND id NOT IN (SELECT child FROM entries)");
    execute(p.db, "DELETE FROM chunks WHERE id NOT IN (SELECT chunk FROM refs)");
    auto count = sqlite3_changes(p.db);
    tx.commit();
    p.clear_cache();
    sync();
    execute(p.db, "VACUUM");
    return count;
}
Stats Store::stats() {
    auto &p = *impl_;
    std::lock_guard guard(p.mutex);
    Stats s;
    s.inodes = scalar(p.db, "SELECT COUNT(*) FROM inodes");
    s.versions = scalar(p.db, "SELECT COUNT(*) FROM versions");
    s.chunks = scalar(p.db, "SELECT COUNT(*) FROM chunks");
    s.references = scalar(p.db, "SELECT COUNT(*) FROM refs");
    s.plaintext_bytes = scalar(p.db, "SELECT COALESCE(SUM(raw_size),0) FROM chunks");
    s.stored_bytes = scalar(p.db, "SELECT COALESCE(SUM(length(data)),0) FROM chunks");
    s.chunk_size = p.chunk_size;
    s.cache_bytes = p.cache_bytes;
    s.cache_hits = p.hits;
    s.cache_misses = p.misses;
    return s;
}
std::vector<std::string> Store::check() {
    auto &p = *impl_;
    std::lock_guard guard(p.mutex);
    std::vector<std::string> errors;
    try {
        Statement integrity(p.db, "PRAGMA integrity_check");
        while (integrity.row())
            if (integrity.text(0) != "ok")
                errors.push_back(integrity.text(0));
        Statement foreign(p.db, "PRAGMA foreign_key_check");
        while (foreign.row())
            errors.push_back("Foreign key violation in " + foreign.text(0));
        if (!S_ISDIR(p.node(1).mode))
            errors.push_back("Root is not a directory");
        Statement nodes(p.db, "SELECT id,mode,size,target,current,uid,gid FROM inodes");
        while (nodes.row()) {
            auto mode = nodes.number(1);
            if ((!S_ISREG(mode) && !S_ISDIR(mode) && !S_ISLNK(mode)) || nodes.number(5) < 0 ||
                nodes.number(6) < 0 ||
                (S_ISLNK(mode) && nodes.number(2) != static_cast<Id>(nodes.text(3).size())) ||
                (!S_ISREG(mode) && nodes.number(4) != 0))
                errors.push_back("Invalid inode metadata " + std::to_string(nodes.number(0)));
        }
        Statement versions(p.db, "SELECT v.id,i.mode FROM versions v JOIN inodes i ON v.inode=i.id");
        while (versions.row()) {
            if (!S_ISREG(versions.number(1)))
                errors.push_back("Version belongs to a non-regular inode");
            try {
                p.verify_version(versions.number(0));
            } catch (const Error &e) {
                errors.push_back(e.what());
            }
        }
        Statement current(p.db,
                          "SELECT i.id FROM inodes i LEFT JOIN versions v ON i.current=v.id WHERE (i.mode & "
                          "61440)=32768 AND (v.id IS NULL OR v.inode<>i.id OR v.size<>i.size)");
        while (current.row())
            errors.push_back("Invalid current version for inode " + std::to_string(current.number(0)));
        Statement refs(
            p.db, "SELECT r.version,r.idx FROM refs r JOIN versions v ON v.id=r.version JOIN chunks c ON "
                  "c.id=r.chunk WHERE r.idx<0 OR r.idx>=(v.size / ? + (v.size % ? != 0)) OR c.raw_size<>?");
        refs.bind(1, static_cast<Id>(p.chunk_size));
        refs.bind(2, static_cast<Id>(p.chunk_size));
        refs.bind(3, static_cast<Id>(p.chunk_size));
        while (refs.row())
            errors.push_back("Chunk outside version size " + std::to_string(refs.number(0)));
        Statement entries(p.db, "SELECT parent,name,child FROM entries");
        while (entries.row()) {
            auto parent = entries.number(0);
            auto name = entries.text(1);
            auto child = entries.number(2);
            if (!S_ISDIR(p.node(parent).mode) || name.empty() || name.size() > 255 || name == "." ||
                name == ".." || name.find('/') != name.npos || name.find('\0') != name.npos || child == 1)
                errors.push_back("Invalid directory entry");
            try {
                p.no_cycle(child, parent);
            } catch (const Error &e) {
                errors.push_back(e.what());
            }
        }
        Statement chunks(p.db, "SELECT id FROM chunks");
        while (chunks.row()) {
            auto id = chunks.text(0);
            try {
                p.load(id, true);
            } catch (const Error &e) {
                errors.push_back("Chunk " + id + ": " + e.what());
            }
        }
    } catch (const std::exception &e) {
        errors.push_back(e.what());
    }
    return errors;
}
void Store::set_commit_hook(std::function<void(std::string_view)> hook) {
    auto &p = *impl_;
    std::lock_guard guard(p.mutex);
    p.hook = std::move(hook);
}
} // namespace localfs
