#include "bootstrap.h"
#include <algorithm>
#include <exception>
#include <stdexcept>

namespace ct::helper {
const Bytes CameraPatch::Original{0x00, 0xd4, 0x60, 0x1e, 0xa7, 0xfd, 0xff, 0x54};
const Bytes CameraPatch::Corrected{0x01, 0xd4, 0x60, 0x1e, 0xa6, 0xfd, 0xff, 0x54};
void CameraPatch::apply() {
    if (memory_.read(Address, 8) != Original)
        throw std::runtime_error("native camera predicate differs from audited instructions");
    try {
        patch_words(memory_, PatchStorage::Code, Address, Corrected);
    } catch (...) {
        const auto failure = std::current_exception();
        restore();
        std::rethrow_exception(failure);
    }
}
void CameraPatch::restore() {
    const auto value = memory_.read(Address, 8);
    if (value.size() != 8)
        throw std::runtime_error("short native camera predicate");
    for (size_t offset = 0; offset < 8; offset += 4)
        if (!std::equal(value.begin() + offset, value.begin() + offset + 4,
                        Original.begin() + offset) &&
            !std::equal(value.begin() + offset, value.begin() + offset + 4,
                        Corrected.begin() + offset))
            throw std::runtime_error(
                "native camera predicate changed externally. Preserve it for review");
    if (value != Original)
        patch_words(memory_, PatchStorage::Code, Address, Original);
}
} // namespace ct::helper
