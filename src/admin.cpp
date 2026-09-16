#include "store.hpp"
#include <cerrno>
#include <fstream>
#include <iostream>
#include <map>
#include <set>
#include <unistd.h>
#include <vector>

namespace {
void usage() {
    std::cout << "fusefs-admin COMMAND STORE --key-file KEY [OPTIONS]\n"
                 "  init     [--chunk-size BYTES] [--compression 0..19] (creates key if absent)\n"
                 "  stats    JSON storage statistics\n"
                 "  fsck     Check SQLite, namespace, references, authentication and hashes\n"
                 "  history  --path /FILE | --inode ID\n"
                 "  restore  --path /FILE --version ID\n"
                 "  cat      --path /FILE [--version ID] (streams to stdout)\n"
                 "  put      --path /FILE --input LOCAL_FILE (streaming import)\n"
                 "  prune    --keep N (at least one version per inode)\n"
                 "  gc       [--purge-deleted] (reclaim chunks and compact DB)\n"
                 "Unmount before administration. Back up the store and key separately.\n";
}
std::uint64_t number(const std::string &value) {
    if (value.empty() || value[0] == '-')
        throw localfs::Error(EINVAL, "Expected an unsigned integer");
    std::size_t end = 0;
    auto n = std::stoull(value, &end);
    if (end != value.size() || n > INT64_MAX)
        throw localfs::Error(EINVAL, "Invalid integer");
    return n;
}
} // namespace
int main(int argc, char **argv) {
    try {
        if (argc == 2 && std::string(argv[1]) == "--version") {
            std::cout << "fusefs-admin 0.2.0\n";
            return 0;
        }
        if (argc < 3) {
            usage();
            return argc == 2 && std::string(argv[1]) == "--help" ? 0 : 1;
        }
        std::string command = argv[1], directory = argv[2];
        std::map<std::string, std::string> options;
        const std::map<std::string, std::set<std::string>> allowed{
            {"init", {"--key-file", "--chunk-size", "--compression"}},
            {"stats", {"--key-file"}},
            {"fsck", {"--key-file"}},
            {"history", {"--key-file", "--path", "--inode"}},
            {"restore", {"--key-file", "--path", "--version"}},
            {"cat", {"--key-file", "--path", "--version"}},
            {"put", {"--key-file", "--path", "--input"}},
            {"prune", {"--key-file", "--keep"}},
            {"gc", {"--key-file", "--purge-deleted"}}};
        if (!allowed.contains(command))
            throw localfs::Error(EINVAL, "Unknown command");
        for (int i = 3; i < argc; ++i) {
            std::string arg = argv[i];
            if (!allowed.at(command).contains(arg) || options.contains(arg))
                throw localfs::Error(EINVAL, "Unknown or duplicate option: " + arg);
            if (arg == "--purge-deleted")
                options[arg] = "true";
            else {
                if (++i == argc)
                    throw localfs::Error(EINVAL, "Missing option value");
                options[arg] = argv[i];
            }
        }
        auto required = [&](const std::string &name) -> const std::string & {
            auto it = options.find(name);
            if (it == options.end() || it->second.empty())
                throw localfs::Error(EINVAL, "Required option: " + name);
            return it->second;
        };
        const auto &key = required("--key-file");
        if (command == "init") {
            auto chunk_size = options.contains("--chunk-size") ? number(options.at("--chunk-size")) : 65536;
            auto compression = options.contains("--compression") ? number(options.at("--compression")) : 3;
            if (compression > 19)
                throw localfs::Error(EINVAL, "Compression must be 0..19");
            localfs::Store::initialize(directory, key, chunk_size, static_cast<int>(compression));
            std::cout << "Store initialized\n";
            return 0;
        }
        localfs::Store store(directory, key);
        auto target = [&] {
            if (options.contains("--inode"))
                return static_cast<localfs::Id>(number(options.at("--inode")));
            return store.lookup(required("--path"));
        };
        if (command == "stats") {
            auto s = store.stats();
            std::cout << "{\"inodes\":" << s.inodes << ",\"versions\":" << s.versions
                      << ",\"chunks\":" << s.chunks << ",\"references\":" << s.references
                      << ",\"plaintext_bytes\":" << s.plaintext_bytes
                      << ",\"stored_bytes\":" << s.stored_bytes << ",\"chunk_size\":" << s.chunk_size
                      << "}\n";
        } else if (command == "fsck") {
            auto errors = store.check();
            for (auto &error : errors)
                std::cerr << error << '\n';
            if (!errors.empty())
                return 2;
            std::cout << "OK: SQLite, namespace, versions, chunks and authentication\n";
        } else if (command == "history") {
            if (options.contains("--path") == options.contains("--inode"))
                throw localfs::Error(EINVAL, "Specify exactly one of --path or --inode");
            auto versions = store.history(target());
            std::cout << '[';
            for (std::size_t i = 0; i < versions.size(); ++i) {
                auto v = versions[i];
                if (i)
                    std::cout << ',';
                std::cout << "{\"id\":" << v.id << ",\"size\":" << v.size
                          << ",\"created_ns\":" << v.created_ns << '}';
            }
            std::cout << "]\n";
        } else if (command == "restore") {
            store.restore(target(), static_cast<localfs::Id>(number(required("--version"))));
            std::cout << "Version restored as a new revision\n";
        } else if (command == "prune") {
            std::cout << store.prune(number(required("--keep"))) << " versions pruned\n";
        } else if (command == "gc") {
            std::cout << store.gc(options.contains("--purge-deleted")) << " chunks reclaimed\n";
        } else if (command == "cat") {
            auto id = target();
            auto version =
                options.contains("--version") ? static_cast<localfs::Id>(number(options.at("--version"))) : 0;
            std::vector<char> buffer(1024 * 1024);
            localfs::Id offset = 0;
            for (;;) {
                auto count = store.read(id, buffer.data(), buffer.size(), offset, version);
                if (!count)
                    break;
                std::cout.write(buffer.data(), static_cast<std::streamsize>(count));
                if (!std::cout)
                    throw localfs::Error(EIO, "Output failed");
                offset += static_cast<localfs::Id>(count);
            }
        } else if (command == "put") {
            std::ifstream input(required("--input"), std::ios::binary);
            if (!input)
                throw localfs::Error(ENOENT, "Cannot open input");
            const auto &path = required("--path");
            localfs::Id id;
            try {
                id = store.lookup(path);
                store.truncate(id, 0);
            } catch (const localfs::Error &e) {
                if (e.code != ENOENT)
                    throw;
                id = store.create(path, S_IFREG | 0600, getuid(), getgid());
            }
            std::vector<char> buffer(1024 * 1024);
            localfs::Id offset = 0;
            while (input) {
                input.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
                auto count = input.gcount();
                if (count) {
                    store.write(id, buffer.data(), static_cast<std::size_t>(count), offset);
                    offset += count;
                }
            }
            if (!input.eof())
                throw localfs::Error(EIO, "Input read failed");
        }
        return 0;
    } catch (const std::exception &e) {
        std::cerr << "fusefs-admin: " << e.what() << '\n';
        return 1;
    }
}
