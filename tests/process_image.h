#pragma once
#include "platform/linux/files.h"
#include "platform/linux/process.h"
#include <cassert>
#include <cerrno>
#include <fcntl.h>
#include <sys/mman.h>

namespace ct::test {
class ProcessImage {
    FileDescriptor image_;
    std::string fingerprint_;

  public:
    explicit ProcessImage(const std::string &name = "controller-utils-process-test") {
        unsigned flags = MFD_CLOEXEC | MFD_ALLOW_SEALING;
#ifdef MFD_EXEC
        flags |= MFD_EXEC;
#endif
        image_.reset(memfd_create(name.c_str(), flags));
        if (!image_ && errno == EINVAL)
            image_.reset(memfd_create(name.c_str(), MFD_CLOEXEC | MFD_ALLOW_SEALING));
        assert(image_);
        const auto binary =
            read_file(std::filesystem::read_symlink("/proc/self/exe"), 128 * 1024 * 1024);
        size_t copied = 0;
        while (copied < binary.size()) {
            const auto count = write(image_.get(), binary.data() + copied, binary.size() - copied);
            if (count < 0 && errno == EINTR)
                continue;
            assert(count > 0);
            copied += size_t(count);
        }
        assert(!fcntl(image_.get(), F_ADD_SEALS,
                      F_SEAL_WRITE | F_SEAL_GROW | F_SEAL_SHRINK | F_SEAL_SEAL));
        fingerprint_ = sha256(image_.get());
        assert(lseek(image_.get(), 0, SEEK_CUR) == off_t(binary.size()));
    }
    std::string launch() const {
        return "/proc/" + std::to_string(getpid()) + "/fd/" + std::to_string(image_.get());
    }
    ProcessSpec spec(pid_t pid) const {
        return {std::filesystem::read_symlink("/proc/" + std::to_string(pid) + "/exe"),
                fingerprint_, getuid(), std::nullopt};
    }
};
} // namespace ct::test
