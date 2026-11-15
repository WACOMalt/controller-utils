#include "records.h"
#include "files.h"
#include <atomic>
#include <cerrno>
#include <system_error>
#include <fcntl.h>

namespace ct {
namespace {
void check(int value, const char *operation) {
    if (value < 0)
        throw std::system_error(errno, std::generic_category(), operation);
}
void filename(const std::string &name) {
    if (name.empty() || name == "." || name == ".." || name.find('/') != std::string::npos ||
        name.find('\0') != std::string::npos)
        throw std::invalid_argument("Invalid protected record name");
}
bool exists(const ProtectedDirectory &directory, const std::string &name) {
    filename(name);
    directory.verify();
    struct stat image{};
    if (!fstatat(directory.descriptor(), name.c_str(), &image, AT_SYMLINK_NOFOLLOW))
        return true;
    if (errno != ENOENT)
        check(-1, "inspect protected record");
    return false;
}
} // namespace
std::optional<std::string> read_record(const ProtectedDirectory &directory, const std::string &name,
                                       size_t limit) {
    if (!exists(directory, name))
        return std::nullopt;
    const auto file = directory.file(name, limit);
    const auto bytes = read_file(file.get(), limit);
    directory.verify_file(name, file.get());
    return bytes;
}
void write_record(const ProtectedDirectory &directory, const std::string &name,
                  const std::string &bytes, uint32_t mode) {
    filename(name);
    if (bytes.size() > MaxRecordSize || (mode & ~0777u) || !(mode & 0400) || (mode & 0022))
        throw std::invalid_argument("Invalid protected record size or permissions");
    if (exists(directory, name))
        directory.file(name, MaxRecordSize);
    static std::atomic<uint64_t> serial{0};
    const auto pending =
        ".pending-" + std::to_string(getpid()) + "-" + std::to_string(serial.fetch_add(1));
    FileDescriptor output(openat(directory.descriptor(), pending.c_str(),
                                 O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, mode));
    if (!output)
        check(-1, "create protected record");
    try {
        check(fchmod(output.get(), mode), "set protected record permissions");
        size_t written = 0;
        while (written < bytes.size()) {
            const auto count = write(output.get(), bytes.data() + written, bytes.size() - written);
            if (count < 0 && errno == EINTR)
                continue;
            if (count <= 0)
                throw std::system_error(count < 0 ? errno : EIO, std::generic_category(),
                                        "write protected record");
            written += size_t(count);
        }
        check(fsync(output.get()), "flush protected record");
        directory.verify_file(pending, output.get());
        check(
            renameat(directory.descriptor(), pending.c_str(), directory.descriptor(), name.c_str()),
            "replace protected record");
        check(fsync(directory.descriptor()), "flush protected record directory");
    } catch (...) {
        struct stat opened{}, current{};
        if (!fstat(output.get(), &opened) &&
            !fstatat(directory.descriptor(), pending.c_str(), &current, AT_SYMLINK_NOFOLLOW) &&
            opened.st_dev == current.st_dev && opened.st_ino == current.st_ino)
            unlinkat(directory.descriptor(), pending.c_str(), 0);
        throw;
    }
}
void erase_record(const ProtectedDirectory &directory, const std::string &name) {
    if (!exists(directory, name))
        return;
    const auto file = directory.file(name, MaxRecordSize);
    directory.verify_file(name, file.get());
    check(unlinkat(directory.descriptor(), name.c_str(), 0), "remove protected record");
    check(fsync(directory.descriptor()), "flush removed protected record");
}
} // namespace ct
