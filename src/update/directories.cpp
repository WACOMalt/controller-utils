#include "directories.h"
#include <cerrno>
#include <system_error>
#include <fcntl.h>

namespace ct::update {
namespace {
void check(int value, const char *operation) {
    if (value < 0)
        throw std::system_error(errno, std::generic_category(), operation);
}
void filename(const std::string &name) {
    if (name.empty() || name == "." || name == ".." || name.find('/') != std::string::npos ||
        name.find('\0') != std::string::npos)
        throw std::invalid_argument("Invalid update directory name");
}
} // namespace
std::optional<DirectoryIdentity> directory_identity(const ProtectedDirectory &parent,
                                                    const std::string &name) {
    filename(name);
    parent.verify();
    struct stat image{};
    if (fstatat(parent.descriptor(), name.c_str(), &image, AT_SYMLINK_NOFOLLOW)) {
        if (errno == ENOENT)
            return std::nullopt;
        check(-1, "inspect update directory");
    }
    if (!S_ISDIR(image.st_mode) || image.st_uid != getuid() || (image.st_mode & 0022))
        throw std::runtime_error("Preserve the changed update directory for review");
    return DirectoryIdentity{uint64_t(image.st_dev), uint64_t(image.st_ino)};
}
void remove_directory(const ProtectedDirectory &parent, const std::string &name,
                      std::optional<DirectoryIdentity> expected) {
    const auto current = directory_identity(parent, name);
    if (!current)
        return;
    if (expected && !(*current == *expected))
        throw std::runtime_error("Preserve the changed update directory for review");
    std::filesystem::remove_all(parent.path() / name);
    parent.verify();
    check(fsync(parent.descriptor()), "flush removed update directory");
}
void move_directory(const ProtectedDirectory &from, const std::string &source,
                    const ProtectedDirectory &to, const std::string &destination, unsigned flags) {
    filename(source);
    filename(destination);
    from.verify();
    to.verify();
    check(renameat2(from.descriptor(), source.c_str(), to.descriptor(), destination.c_str(), flags),
          "swap application update");
    check(fsync(from.descriptor()), "flush update source directory");
    check(fsync(to.descriptor()), "flush update destination directory");
}
} // namespace ct::update
