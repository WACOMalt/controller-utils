#pragma once
#include "platform/linux/resource.h"
#include "platform/linux/protected_directory.h"
#include <filesystem>

namespace ct {
class ApplicationLock {
    std::filesystem::path path_;
    ProtectedDirectory directory_;
    FileDescriptor descriptor_;
    bool primary_ = false;

  public:
    explicit ApplicationLock(const std::filesystem::path &path);
    bool primary() const {
        return primary_;
    }
    int descriptor() const {
        return descriptor_.get();
    }
    void publish();
    bool show_running(const std::filesystem::path &executable, double timeout = 3);
};
} // namespace ct
