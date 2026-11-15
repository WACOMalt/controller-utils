#pragma once
#include "resource.h"
#include <filesystem>
#include <string>
#include <sys/stat.h>

namespace ct {
class ProtectedDirectory {
    std::filesystem::path path_;
    uid_t owner_;
    FileDescriptor directory_;
    struct stat image_{};
    FileDescriptor open_directory() const;

  public:
    explicit ProtectedDirectory(std::filesystem::path path, uid_t owner = 0);
    const std::filesystem::path &path() const {
        return path_;
    }
    int descriptor() const {
        return directory_.get();
    }
    uid_t owner() const {
        return owner_;
    }
    void verify() const;
    FileDescriptor file(const std::string &name, size_t limit) const;
    void verify_file(const std::string &name, int descriptor) const;
};
} // namespace ct
