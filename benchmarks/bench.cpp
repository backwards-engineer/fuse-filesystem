#include "store.hpp"
#include <algorithm>
#include <chrono>
#include <filesystem>
#include <iostream>
#include <random>
#include <thread>
#include <unistd.h>
using namespace localfs;
int main(int argc, char **argv) {
    try {
        std::size_t total = 8 * 1024 * 1024, chunk = 65536;
        for (int i = 1; i < argc; ++i) {
            std::string arg = argv[i];
            if (++i == argc)
                throw std::runtime_error("Missing benchmark argument");
            if (arg == "--bytes")
                total = std::stoull(argv[i]);
            else if (arg == "--chunk-size")
                chunk = std::stoull(argv[i]);
            else
                throw std::runtime_error("Unknown benchmark argument");
        }
        if (total < 4096 || total > 1024ULL * 1024 * 1024)
            throw std::runtime_error("--bytes must be 4096..1073741824");
        char path[] = "/tmp/local-fusefs-bench-XXXXXX";
        if (!mkdtemp(path))
            throw std::runtime_error("mkdtemp failed");
        struct Cleanup {
            std::string path;
            ~Cleanup() {
                std::error_code ec;
                std::filesystem::remove_all(path, ec);
            }
        } cleanup{path};
        std::string directory = std::string(path) + "/store", key = std::string(path) + "/key";
        Store::initialize(directory, key, chunk, 3);
        Store s(directory, key, 4 * 1024 * 1024);
        auto id = s.create("/data", S_IFREG | 0600, getuid(), getgid());
        std::vector<unsigned char> data(1024 * 1024);
        std::mt19937 random(17419);
        for (auto &b : data)
            b = static_cast<unsigned char>(random());
        auto seconds = [](auto start) {
            return std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
        };
        auto start = std::chrono::steady_clock::now();
        for (std::size_t offset = 0; offset < total; offset += data.size())
            s.write(id, data.data(), std::min(data.size(), total - offset), static_cast<Id>(offset));
        double write_seconds = seconds(start);
        std::vector<unsigned char> output(data.size());
        auto read_pass = [&] {
            for (std::size_t offset = 0; offset < total; offset += output.size()) {
                auto n = std::min(output.size(), total - offset);
                if (s.read(id, output.data(), n, static_cast<Id>(offset)) != n ||
                    !std::equal(output.begin(), output.begin() + static_cast<std::ptrdiff_t>(n),
                                data.begin()))
                    throw std::runtime_error("Benchmark data mismatch");
            }
        };
        start = std::chrono::steady_clock::now();
        read_pass();
        double read_seconds = seconds(start);
        start = std::chrono::steady_clock::now();
        read_pass();
        double cached_seconds = seconds(start);
        start = std::chrono::steady_clock::now();
        for (int i = 0; i < 100; ++i) {
            auto off = static_cast<Id>(random() % (total - 4095));
            s.write(id, data.data(), 4096, off);
        }
        double random_seconds = seconds(start);
        std::vector<Id> ids;
        for (int i = 0; i < 4; ++i)
            ids.push_back(s.create("/thread" + std::to_string(i), S_IFREG | 0600, getuid(), getgid()));
        std::vector<std::thread> workers;
        start = std::chrono::steady_clock::now();
        for (auto target : ids)
            workers.emplace_back([&, target] {
                for (int i = 0; i < 8; ++i)
                    s.write(target, data.data(), 65536, i * 65536);
            });
        for (auto &thread : workers) {
            thread.join();
        }
        double concurrent_seconds = seconds(start);
        auto st = s.stats();
        double mib = static_cast<double>(total) / (1024 * 1024);
        std::cout << "{\"bytes\":" << total << ",\"chunk_size\":" << chunk
                  << ",\"sequential_write_mib_s\":" << mib / write_seconds
                  << ",\"sequential_read_mib_s\":" << mib / read_seconds
                  << ",\"cached_read_mib_s\":" << mib / cached_seconds
                  << ",\"random_4k_write_iops\":" << 100 / random_seconds
                  << ",\"four_thread_write_mib_s\":" << 2 / concurrent_seconds
                  << ",\"unique_chunks\":" << st.chunks << ",\"chunk_references\":" << st.references
                  << ",\"stored_bytes\":" << st.stored_bytes << ",\"cache_hits\":" << st.cache_hits
                  << ",\"cache_misses\":" << st.cache_misses << "}\n";
        return 0;
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
