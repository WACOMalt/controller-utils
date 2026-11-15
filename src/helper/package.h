#pragma once
#include "adapter_patch.h"
#include "native_paths.h"
#include "platform/linux/protected_directory.h"

namespace ct::helper {
class NativePackage {
    ProtectedDirectory directory_;
    FileDescriptor library_;
    struct stat library_image_{};
    AdapterLayout layout_;

  public:
    static constexpr const char *Root = TrackerDirectory;
    static constexpr const char *Library = TrackerLibrary;
    explicit NativePackage(std::filesystem::path path = Root, uid_t owner = 0);
    const AdapterLayout &layout() const {
        return layout_;
    }
    void verify(const std::vector<MemoryMapping> &mappings) const;
};
} // namespace ct::helper
