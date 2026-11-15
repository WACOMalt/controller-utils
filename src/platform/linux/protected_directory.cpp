#include "protected_directory.h"
#include "error.h"
#include <cerrno>
#include <stdexcept>
#include <system_error>
#include <fcntl.h>

namespace ct {
namespace {
void check(int value, const char *operation) {
    if (value < 0)
        throw std::system_error(errno, std::generic_category(), operation);
}
void name(const std::string &value) {
    if (value.empty() || value == "." || value == ".." || value.find('/') != std::string::npos ||
        value.find('\0') != std::string::npos)
        throw std::invalid_argument("invalid protected filename");
}
bool same_file(const struct stat &a, const struct stat &b) {
    return a.st_dev == b.st_dev && a.st_ino == b.st_ino && a.st_size == b.st_size &&
           a.st_mode == b.st_mode && a.st_uid == b.st_uid && a.st_gid == b.st_gid &&
           a.st_nlink == b.st_nlink && a.st_mtim.tv_sec == b.st_mtim.tv_sec &&
           a.st_mtim.tv_nsec == b.st_mtim.tv_nsec && a.st_ctim.tv_sec == b.st_ctim.tv_sec &&
           a.st_ctim.tv_nsec == b.st_ctim.tv_nsec;
}
} // namespace
FileDescriptor ProtectedDirectory::open_directory() const {
    FileDescriptor current(open("/", O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
    if (!current)
        check(-1, "open protected directory root");
    struct stat info{};
    auto validate = [&] {
        check(fstat(current.get(), &info), "stat protected directory");
        if (!S_ISDIR(info.st_mode) || (info.st_uid != 0 && info.st_uid != owner_) ||
            (info.st_mode & 0022))
            throw std::runtime_error("Unprotected native adapter path: " + path_.string());
    };
    validate();
    for (const auto &part : path_.relative_path()) {
        const auto component = part.string();
        name(component);
        FileDescriptor next(openat(current.get(), component.c_str(),
                                   O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
        if (!next)
            check(-1, "open protected directory component");
        current = std::move(next);
        validate();
    }
    if (info.st_uid != owner_)
        throw std::runtime_error("protected directory owner differs");
    return current;
}
ProtectedDirectory::ProtectedDirectory(std::filesystem::path path, uid_t owner)
    : path_(std::move(path)), owner_(owner) {
    if (!path_.is_absolute() || path_.lexically_normal() != path_ ||
        path_.string().find('\0') != std::string::npos)
        throw std::invalid_argument("invalid protected directory path");
    directory_ = open_directory();
    check(fstat(directory_.get(), &image_), "pin protected directory");
}
void ProtectedDirectory::verify() const {
    const auto current = open_directory();
    struct stat info{};
    check(fstat(current.get(), &info), "verify protected directory");
    if (info.st_dev != image_.st_dev || info.st_ino != image_.st_ino)
        throw ConnectionError("protected directory identity changed");
}
FileDescriptor ProtectedDirectory::file(const std::string &filename, size_t limit) const {
    name(filename);
    verify();
    FileDescriptor value(
        openat(directory_.get(), filename.c_str(), O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC));
    if (!value)
        check(-1, "open protected file");
    struct stat info{};
    check(fstat(value.get(), &info), "stat protected file");
    if (!S_ISREG(info.st_mode) || info.st_uid != owner_ || (info.st_mode & 0022) ||
        info.st_nlink != 1 || info.st_size < 0 || uint64_t(info.st_size) > limit)
        throw std::runtime_error("unprotected or oversized native adapter file");
    verify_file(filename, value.get());
    return value;
}
void ProtectedDirectory::verify_file(const std::string &filename, int descriptor) const {
    name(filename);
    verify();
    struct stat pinned{}, current{};
    check(fstat(descriptor, &pinned), "stat pinned protected file");
    check(fstatat(directory_.get(), filename.c_str(), &current, AT_SYMLINK_NOFOLLOW),
          "verify protected filename");
    if (!S_ISREG(pinned.st_mode) || pinned.st_uid != owner_ || (pinned.st_mode & 0022) ||
        pinned.st_nlink != 1 || !same_file(pinned, current))
        throw ConnectionError("protected file identity changed");
}
} // namespace ct
