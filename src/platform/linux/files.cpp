#include "files.h"
#include "resource.h"
#include <array>
#include <atomic>
#include <cerrno>
#include <fcntl.h>
#include <memory>
#include <openssl/evp.h>
#include <stdexcept>
#include <system_error>
#include <sys/stat.h>
#include <unistd.h>

namespace ct {
namespace {
FileDescriptor input_file(const std::filesystem::path &path) {
    FileDescriptor descriptor(open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NONBLOCK));
    if (!descriptor)
        throw std::system_error(errno, std::generic_category(), "open " + path.string());
    struct stat info{};
    if (fstat(descriptor.get(), &info))
        throw std::system_error(errno, std::generic_category(), "stat " + path.string());
    if (!S_ISREG(info.st_mode))
        throw std::invalid_argument("expected a regular file: " + path.string());
    return descriptor;
}
ssize_t read_retry(int descriptor, void *buffer, size_t size) {
    ssize_t count;
    do {
        count = ::read(descriptor, buffer, size);
    } while (count < 0 && errno == EINTR);
    if (count < 0)
        throw std::system_error(errno, std::generic_category(), "file read");
    return count;
}
} // namespace
std::string sha256_bytes(const void *data, size_t size) {
    if (!data && size)
        throw std::invalid_argument("invalid digest input");
    std::array<unsigned char, 32> digest{};
    unsigned length = 0;
    if (EVP_Digest(data, size, digest.data(), &length, EVP_sha256(), nullptr) != 1 ||
        length != digest.size())
        throw std::runtime_error("cannot calculate SHA-256");
    static constexpr char digits[] = "0123456789abcdef";
    std::string result;
    for (auto byte : digest) {
        result += digits[byte >> 4];
        result += digits[byte & 15];
    }
    return result;
}
std::string read_file(const std::filesystem::path &path, size_t limit) {
    auto descriptor = input_file(path);
    std::string contents;
    std::array<char, 65536> buffer;
    while (const auto count = read_retry(descriptor.get(), buffer.data(), buffer.size())) {
        if (size_t(count) > limit - contents.size())
            throw std::invalid_argument("file exceeds its size limit: " + path.string());
        contents.append(buffer.data(), size_t(count));
    }
    return contents;
}
std::string sha256(const std::filesystem::path &path) {
    auto descriptor = input_file(path);
    return sha256(descriptor.get());
}
std::string read_file(int descriptor, size_t limit) {
    struct stat info{};
    if (fstat(descriptor, &info))
        throw std::system_error(errno, std::generic_category(), "read file identity");
    if (!S_ISREG(info.st_mode))
        throw std::invalid_argument("expected a regular file");
    std::string result;
    std::array<char, 65536> buffer;
    off_t offset = 0;
    for (;;) {
        const auto count = pread(descriptor, buffer.data(), buffer.size(), offset);
        if (count < 0) {
            if (errno == EINTR)
                continue;
            throw std::system_error(errno, std::generic_category(), "read bounded file");
        }
        if (!count)
            return result;
        if (size_t(count) > limit - result.size())
            throw std::invalid_argument("file exceeds its size limit");
        result.append(buffer.data(), size_t(count));
        offset += count;
    }
}
std::string sha256(int descriptor) {
    struct stat info{};
    if (fstat(descriptor, &info))
        throw std::system_error(errno, std::generic_category(), "hash file identity");
    if (!S_ISREG(info.st_mode))
        throw std::invalid_argument("expected a regular hash input");
    std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)> context(EVP_MD_CTX_new(),
                                                                    EVP_MD_CTX_free);
    if (!context || EVP_DigestInit_ex(context.get(), EVP_sha256(), nullptr) != 1)
        throw std::runtime_error("cannot initialize SHA-256");
    std::array<char, 65536> buffer;
    off_t offset = 0;
    for (;;) {
        const auto count = pread(descriptor, buffer.data(), buffer.size(), offset);
        if (count < 0 && errno == EINTR)
            continue;
        if (count < 0)
            throw std::system_error(errno, std::generic_category(), "hash file read");
        if (!count)
            break;
        if (EVP_DigestUpdate(context.get(), buffer.data(), size_t(count)) != 1)
            throw std::runtime_error("cannot update SHA-256");
        offset += count;
    }
    std::array<unsigned char, 32> digest{};
    unsigned size = 0;
    if (EVP_DigestFinal_ex(context.get(), digest.data(), &size) != 1 || size != digest.size())
        throw std::runtime_error("cannot finalize SHA-256");
    static constexpr char digits[] = "0123456789abcdef";
    std::string result;
    for (auto byte : digest) {
        result += digits[byte >> 4];
        result += digits[byte & 15];
    }
    return result;
}
void atomic_write(const std::filesystem::path &path, const std::string &contents, uint32_t mode) {
    if (mode & ~0777u)
        throw std::invalid_argument("invalid file permissions");
    const auto directory =
        path.parent_path().empty() ? std::filesystem::path(".") : path.parent_path();
    std::filesystem::create_directories(directory);
    static std::atomic<uint64_t> serial{0};
    const auto temporary =
        directory / (path.filename().string() + ".pending-" + std::to_string(getpid()) + "-" +
                     std::to_string(serial.fetch_add(1)));
    FileDescriptor output(
        open(temporary.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, mode));
    if (!output)
        throw std::system_error(errno, std::generic_category(), "create atomic file");
    try {
        if (fchmod(output.get(), mode))
            throw std::system_error(errno, std::generic_category(), "atomic file permissions");
        size_t written = 0;
        while (written < contents.size()) {
            const auto count =
                ::write(output.get(), contents.data() + written, contents.size() - written);
            if (count < 0 && errno == EINTR)
                continue;
            if (count <= 0)
                throw std::system_error(count < 0 ? errno : EIO, std::generic_category(),
                                        "atomic file write");
            written += size_t(count);
        }
        if (fsync(output.get()))
            throw std::system_error(errno, std::generic_category(), "atomic file flush");
        output.reset();
        if (rename(temporary.c_str(), path.c_str()))
            throw std::system_error(errno, std::generic_category(), "atomic file replacement");
        FileDescriptor parent(open(directory.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC));
        if (!parent || fsync(parent.get()))
            throw std::system_error(errno, std::generic_category(), "atomic directory flush");
    } catch (...) {
        unlink(temporary.c_str());
        throw;
    }
}
} // namespace ct
