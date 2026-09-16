#pragma once
#include <array>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace localfs {
using Bytes = std::vector<unsigned char>;
using Key = std::array<unsigned char, 32>;
Key read_key(const std::string &path);
void create_key(const std::string &path);
std::string digest(std::span<const unsigned char> data);
std::string key_check(const Key &key, const std::string &configuration);
Bytes seal(const Key &key, const std::string &aad, std::span<const unsigned char> data);
Bytes unseal(const Key &key, const std::string &aad, std::span<const unsigned char> data);
} // namespace localfs
