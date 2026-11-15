#pragma once
#include "resource.h"
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <type_traits>
#include <vector>
#include <sys/stat.h>

namespace ct {
namespace helper {
class GateMemory;
}
struct ProcessStamp {
    char state;
    uint64_t started;
};
ProcessStamp process_stamp(const std::string &text);
struct MemoryMapping {
    uintptr_t first, last, offset;
    dev_t device;
    uint64_t inode;
    bool readable, writable, executable;
    std::string path;
    bool private_mapping = true;
    bool contains(uintptr_t address, size_t size) const;
};
std::vector<MemoryMapping> process_mappings(const std::string &text);
std::vector<MemoryMapping> stable_process_mappings(const std::function<std::string()> &read);
struct ProcessSpec {
    std::filesystem::path executable;
    std::string sha256;
    uid_t owner;
    std::optional<uintptr_t> image_base;
};
class PinnedProcess {
    friend class helper::GateMemory;
    pid_t pid_;
    ProcessSpec spec_;
    FileDescriptor pidfd_, directory_, executable_, memory_;
    struct stat image_{};
    uint64_t started_ = 0;
    bool writable_;
    std::optional<uintptr_t> image_base_;
    std::vector<MemoryMapping> mappings_;
    void verify() const;
    void range(uintptr_t address, size_t size, bool writing) const;

  public:
    PinnedProcess(pid_t pid, ProcessSpec spec, bool writable = false);
    PinnedProcess(const PinnedProcess &) = delete;
    PinnedProcess &operator=(const PinnedProcess &) = delete;
    pid_t pid() const {
        return pid_;
    }
    int pid_descriptor() const {
        return pidfd_.get();
    }
    std::filesystem::path proc() const;
    uint64_t started() const {
        return started_;
    }
    uintptr_t image_base() const {
        return image_base_.value();
    }
    const std::vector<MemoryMapping> &mappings() const {
        return mappings_;
    }
    bool alive() const;
    void refresh();
    std::string image() const;
    std::vector<uint8_t> read(uintptr_t address, size_t size) const;
    void write_data(uintptr_t address, const std::vector<uint8_t> &bytes);
    template <class T> T field(uintptr_t address) const {
        static_assert(std::is_trivially_copyable_v<T>);
        const auto bytes = read(address, sizeof(T));
        T value;
        std::memcpy(&value, bytes.data(), sizeof(value));
        return value;
    }
};
} // namespace ct
