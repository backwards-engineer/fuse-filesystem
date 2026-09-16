#include "crypto.hpp"
#include "store.hpp"
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <filesystem>
#include <memory>
#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/hmac.h>
#include <openssl/rand.h>
#include <unistd.h>

namespace localfs {
namespace {
void require(bool ok, const char *message) {
    if (!ok)
        throw Error(EIO, message);
}
std::string hex(std::span<const unsigned char> input) {
    static const char digits[] = "0123456789abcdef";
    std::string result;
    for (auto c : input) {
        result += digits[c >> 4];
        result += digits[c & 15];
    }
    return result;
}
struct Fd {
    int fd;
    ~Fd() {
        if (fd >= 0)
            ::close(fd);
    }
};
} // namespace
Key read_key(const std::string &path) {
    Fd file{::open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW)};
    if (file.fd < 0)
        throw Error(errno, "Cannot open key file");
    struct stat st {};
    if (::fstat(file.fd, &st) != 0)
        throw Error(errno, "Cannot inspect key file");
    if (!S_ISREG(st.st_mode) || st.st_size != 32 || (st.st_mode & 077) || st.st_uid != ::geteuid())
        throw Error(EACCES, "Key must be a 32-byte regular file owned by you with mode 0600");
    Key key{};
    require(::read(file.fd, key.data(), key.size()) == static_cast<ssize_t>(key.size()), "Cannot read key");
    return key;
}
void create_key(const std::string &path) {
    Key key{};
    require(RAND_bytes(key.data(), static_cast<int>(key.size())) == 1, "Random key generation failed");
    Fd file{::open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600)};
    if (file.fd < 0)
        throw Error(errno, "Cannot create key file (existing keys are never overwritten)");
    bool ok = ::write(file.fd, key.data(), key.size()) == static_cast<ssize_t>(key.size());
    OPENSSL_cleanse(key.data(), key.size());
    require(ok && ::fsync(file.fd) == 0, "Cannot persist key file");
    auto parent = std::filesystem::absolute(path).parent_path();
    Fd directory{::open(parent.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC)};
    require(directory.fd >= 0 && ::fsync(directory.fd) == 0, "Cannot sync key directory");
}
std::string digest(std::span<const unsigned char> data) {
    std::array<unsigned char, 32> result{};
    unsigned length = 0;
    require(EVP_Digest(data.data(), data.size(), result.data(), &length, EVP_sha256(), nullptr) == 1 &&
                length == 32,
            "SHA-256 failed");
    return hex(result);
}
std::string key_check(const Key &key, const std::string &configuration) {
    std::array<unsigned char, 32> result{};
    unsigned length = 0;
    require(HMAC(EVP_sha256(), key.data(), static_cast<int>(key.size()),
                 reinterpret_cast<const unsigned char *>(configuration.data()), configuration.size(),
                 result.data(), &length) != nullptr,
            "Key verification failed");
    return hex(result);
}
Bytes seal(const Key &key, const std::string &aad, std::span<const unsigned char> data) {
    // Version 1 envelope: random 96-bit nonce, ciphertext, 128-bit authentication tag.
    Bytes out(12 + data.size() + 16);
    require(RAND_bytes(out.data(), 12) == 1, "Nonce generation failed");
    std::unique_ptr<EVP_CIPHER_CTX, decltype(&EVP_CIPHER_CTX_free)> ctx(EVP_CIPHER_CTX_new(),
                                                                        EVP_CIPHER_CTX_free);
    require(ctx != nullptr, "Cipher allocation failed");
    int length = 0, written = 0;
    require(EVP_EncryptInit_ex(ctx.get(), EVP_aes_256_gcm(), nullptr, key.data(), out.data()) == 1,
            "Cipher init failed");
    require(EVP_EncryptUpdate(ctx.get(), nullptr, &length,
                              reinterpret_cast<const unsigned char *>(aad.data()),
                              static_cast<int>(aad.size())) == 1,
            "AAD failed");
    require(EVP_EncryptUpdate(ctx.get(), out.data() + 12, &written, data.data(),
                              static_cast<int>(data.size())) == 1,
            "Encryption failed");
    require(EVP_EncryptFinal_ex(ctx.get(), out.data() + 12 + written, &length) == 1,
            "Encryption final failed");
    require(EVP_CIPHER_CTX_ctrl(ctx.get(), EVP_CTRL_GCM_GET_TAG, 16, out.data() + out.size() - 16) == 1,
            "Tag generation failed");
    return out;
}
Bytes unseal(const Key &key, const std::string &aad, std::span<const unsigned char> data) {
    if (data.size() < 28 || data.size() > 8 * 1024 * 1024)
        throw Error(EIO, "Invalid encrypted chunk size");
    Bytes out(data.size() - 28);
    std::unique_ptr<EVP_CIPHER_CTX, decltype(&EVP_CIPHER_CTX_free)> ctx(EVP_CIPHER_CTX_new(),
                                                                        EVP_CIPHER_CTX_free);
    require(ctx != nullptr, "Cipher allocation failed");
    int length = 0, written = 0;
    require(EVP_DecryptInit_ex(ctx.get(), EVP_aes_256_gcm(), nullptr, key.data(), data.data()) == 1,
            "Cipher init failed");
    require(EVP_DecryptUpdate(ctx.get(), nullptr, &length,
                              reinterpret_cast<const unsigned char *>(aad.data()),
                              static_cast<int>(aad.size())) == 1,
            "AAD failed");
    require(EVP_DecryptUpdate(ctx.get(), out.data(), &written, data.data() + 12,
                              static_cast<int>(out.size())) == 1,
            "Decryption failed");
    require(EVP_CIPHER_CTX_ctrl(ctx.get(), EVP_CTRL_GCM_SET_TAG, 16,
                                const_cast<unsigned char *>(data.data() + data.size() - 16)) == 1,
            "Tag loading failed");
    if (EVP_DecryptFinal_ex(ctx.get(), out.data() + written, &length) != 1) {
        OPENSSL_cleanse(out.data(), out.size());
        throw Error(EIO, "Chunk authentication failed (corrupt data or wrong key)");
    }
    return out;
}
} // namespace localfs
