#include "process.h"
#include "error.h"
#include <charconv>
#include <climits>
#include <sstream>
#include <stdexcept>
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
} // namespace
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
        throw ConnectionError("Steam process mappings are unavailable.");
    return result;
}
} // namespace ct
