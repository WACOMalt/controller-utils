#include "process.h"
#include "error.h"
#include "files.h"
#include <algorithm>
#include <cerrno>
#include <charconv>
#include <climits>
#include <limits>
#include <poll.h>
#include <set>
#include <sstream>
#include <stdexcept>
#include <system_error>
#include <fcntl.h>
#include <sys/syscall.h>
#include <sys/sysmacros.h>

namespace ct {
namespace {
class ChangingMappings : public std::invalid_argument {
  public:
    ChangingMappings() : std::invalid_argument("native process mappings changed during read") {}
};
uint64_t number(const std::string &text, int base) {
    uint64_t value = 0;
    const auto result = std::from_chars(text.data(), text.data() + text.size(), value, base);
    if (text.empty() || result.ec != std::errc{} || result.ptr != text.data() + text.size())
        throw std::invalid_argument("invalid native process field");
    return value;
}
bool same_image(const struct stat &a, const struct stat &b) {
    return a.st_dev == b.st_dev && a.st_ino == b.st_ino && a.st_size == b.st_size &&
           a.st_mtim.tv_sec == b.st_mtim.tv_sec && a.st_mtim.tv_nsec == b.st_mtim.tv_nsec &&
           a.st_ctim.tv_sec == b.st_ctim.tv_sec && a.st_ctim.tv_nsec == b.st_ctim.tv_nsec;
}
} // namespace
ProcessStamp process_stamp(const std::string &text) {
    const auto end = text.rfind(')');
    if (end == std::string::npos)
        throw std::invalid_argument("invalid native process identity");
    std::istringstream fields(text.substr(end + 1));
    std::string value, state;
    for (unsigned index = 0; index <= 19; ++index) {
        if (!(fields >> value))
            throw std::invalid_argument("invalid native process identity");
        if (!index)
            state = value;
    }
    if (state.size() != 1)
        throw std::invalid_argument("invalid native process state");
    return {state.front(), number(value, 10)};
}
bool MemoryMapping::contains(uintptr_t address, size_t size) const {
    return size && address >= first && address < last && size <= last - address;
}
std::vector<MemoryMapping> process_mappings(const std::string &text) {
    std::vector<MemoryMapping> result;
    std::istringstream input(text);
    std::string line;
    while (std::getline(input, line)) {
        std::istringstream fields(line);
        std::string addresses, permissions, offset, device, inode, path;
        if (!(fields >> addresses >> permissions >> offset >> device >> inode))
            throw std::invalid_argument("invalid native process mapping");
        const auto dash = addresses.find('-'), colon = device.find(':');
        if (dash == std::string::npos || colon == std::string::npos || permissions.size() != 4 ||
            (permissions[0] != 'r' && permissions[0] != '-') ||
            (permissions[1] != 'w' && permissions[1] != '-') ||
            (permissions[2] != 'x' && permissions[2] != '-') ||
            (permissions[3] != 'p' && permissions[3] != 's'))
            throw std::invalid_argument("invalid native process mapping");
        const auto first = number(addresses.substr(0, dash), 16),
                   last = number(addresses.substr(dash + 1), 16);
        const auto major_id = number(device.substr(0, colon), 16),
                   minor_id = number(device.substr(colon + 1), 16);
        if (first >= last || first > UINTPTR_MAX || last > UINTPTR_MAX || major_id > UINT_MAX ||
            minor_id > UINT_MAX)
            throw std::invalid_argument("invalid native process mapping bounds");
        std::getline(fields, path);
        const auto start = path.find_first_not_of(' ');
        path = start == std::string::npos ? "" : path.substr(start);
        if (!result.empty() && first < result.back().last)
            throw ChangingMappings();
        result.push_back({uintptr_t(first), uintptr_t(last), uintptr_t(number(offset, 16)),
                          makedev(unsigned(major_id), unsigned(minor_id)), number(inode, 10),
                          permissions[0] == 'r', permissions[1] == 'w', permissions[2] == 'x', path,
                          permissions[3] == 'p'});
        if (result.size() > 65536)
            throw std::invalid_argument("native process mapping limit exceeded");
    }
    if (result.empty())
        throw ConnectionError("Native tracking service exited. Reconnect the session.");
    return result;
}
std::filesystem::path PinnedProcess::proc() const {
    return "/proc/self/fd/" + std::to_string(directory_.get());
}
std::vector<MemoryMapping> stable_process_mappings(const std::function<std::string()> &read) {
    if (!read)
        throw std::invalid_argument("missing native mapping reader");
    for (unsigned attempt = 0;; ++attempt) {
        try {
            return process_mappings(read());
        } catch (const ChangingMappings &) {
            if (attempt == 2)
                throw;
        }
    }
}
PinnedProcess::PinnedProcess(pid_t pid, ProcessSpec spec, bool writable)
    : pid_(pid), spec_(std::move(spec)), writable_(writable) {
    if (pid <= 0 || !spec_.executable.is_absolute() || spec_.sha256.size() != 64 ||
        !std::all_of(spec_.sha256.begin(), spec_.sha256.end(),
                     [](char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); }))
        throw std::invalid_argument("invalid native process specification");
    pidfd_.reset(int(syscall(SYS_pidfd_open, pid_, 0)));
    if (!pidfd_)
        throw std::system_error(errno, std::generic_category(), "pin native process");
    directory_.reset(
        open(("/proc/" + std::to_string(pid_)).c_str(), O_PATH | O_DIRECTORY | O_CLOEXEC));
    if (!directory_)
        throw std::system_error(errno, std::generic_category(), "pin native process directory");
    started_ = process_stamp(read_file(proc() / "stat", 16384)).started;
    executable_.reset(openat(directory_.get(), "exe", O_RDONLY | O_CLOEXEC));
    if (!executable_ || fstat(executable_.get(), &image_))
        throw std::system_error(errno, std::generic_category(), "pin native executable");
    verify();
    if (sha256(executable_.get()) != spec_.sha256)
        throw std::runtime_error("unsupported native process binary");
    memory_.reset(openat(directory_.get(), "mem", (writable_ ? O_RDWR : O_RDONLY) | O_CLOEXEC));
    if (!memory_)
        throw std::system_error(errno, std::generic_category(), "pin native process memory");
    refresh();
}
std::string PinnedProcess::image() const {
    verify();
    auto bytes = read_file(executable_.get(), 256u * 1024u * 1024u);
    if (sha256_bytes(bytes.data(), bytes.size()) != spec_.sha256)
        throw std::runtime_error("native executable changed while reading");
    verify();
    return bytes;
}
bool PinnedProcess::alive() const {
    pollfd descriptor{pidfd_.get(), POLLIN, 0};
    int ready;
    do {
        ready = poll(&descriptor, 1, 0);
    } while (ready < 0 && errno == EINTR);
    if (ready < 0)
        throw std::system_error(errno, std::generic_category(), "native process readiness");
    return !ready;
}
void PinnedProcess::verify() const {
    struct stat owner{}, executable{};
    if (!alive() || fstat(directory_.get(), &owner) || owner.st_uid != spec_.owner ||
        process_stamp(read_file(proc() / "stat", 16384)).started != started_ ||
        std::filesystem::read_symlink(proc() / "exe") != spec_.executable ||
        fstatat(directory_.get(), "exe", &executable, 0) || !same_image(image_, executable))
        throw ConnectionError("native process identity changed");
}
void PinnedProcess::refresh() {
    auto next = stable_process_mappings([&] {
        verify();
        return read_file(proc() / "maps", 4 * 1024 * 1024);
    });
    std::set<uintptr_t> bases, code_bases;
    bool code = false;
    for (const auto &mapping : next) {
        if (mapping.inode != uint64_t(image_.st_ino) || mapping.path != spec_.executable.string())
            continue;
        if (mapping.device != image_.st_dev) {
            // Btrfs uses different device IDs in maps and subvolume file metadata.
            std::ostringstream name;
            name << "map_files/" << std::hex << mapping.first << '-' << mapping.last;
            struct stat mapped{};
            if (fstatat(directory_.get(), name.str().c_str(), &mapped, 0) ||
                !same_image(image_, mapped))
                continue;
        }
        if (mapping.offset == 0 && mapping.readable)
            bases.insert(mapping.first);
        if (mapping.offset == 0 && mapping.readable && mapping.executable)
            code_bases.insert(mapping.first);
        code = code || mapping.executable;
    }
    if (!code_bases.empty())
        bases = std::move(code_bases);
    if (bases.size() != 1 || !code || (spec_.image_base && *bases.begin() != *spec_.image_base) ||
        (image_base_ && *bases.begin() != *image_base_))
        throw ConnectionError("native process image layout changed");
    verify();
    image_base_ = *bases.begin();
    mappings_ = std::move(next);
}
void PinnedProcess::range(uintptr_t address, size_t size, bool writing) const {
    if (!size || size > 4096 || address > uintptr_t(std::numeric_limits<off_t>::max()) ||
        !std::any_of(mappings_.begin(), mappings_.end(), [&](const auto &mapping) {
            return mapping.readable && (!writing || mapping.writable) &&
                   mapping.contains(address, size);
        }))
        throw std::invalid_argument("unreadable or out-of-bounds native field");
}
std::vector<uint8_t> PinnedProcess::read(uintptr_t address, size_t size) const {
    if (!alive())
        throw ConnectionError("native process identity changed");
    range(address, size, false);
    std::vector<uint8_t> value(size);
    ssize_t count;
    do {
        count = pread(memory_.get(), value.data(), size, off_t(address));
    } while (count < 0 && errno == EINTR);
    if (count < 0)
        throw std::system_error(errno, std::generic_category(), "read pinned native field");
    if (size_t(count) != size)
        throw ConnectionError("short native field read");
    return value;
}
void PinnedProcess::write_data(uintptr_t address, const std::vector<uint8_t> &bytes) {
    if (!writable_)
        throw std::logic_error("native process memory is read-only");
    refresh();
    range(address, bytes.size(), true);
    ssize_t count;
    do {
        count = pwrite(memory_.get(), bytes.data(), bytes.size(), off_t(address));
    } while (count < 0 && errno == EINTR);
    if (count < 0)
        throw std::system_error(errno, std::generic_category(), "write pinned native field");
    if (size_t(count) != bytes.size() || read(address, bytes.size()) != bytes)
        throw ConnectionError("native field write was not verified");
}
} // namespace ct
