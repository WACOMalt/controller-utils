#pragma once
#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>
#include <openssl/sha.h>

static_assert(__BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__, "Little-endian host required");

namespace ct::steamvr {
inline std::string code_signature(const void *data, size_t size) {
    if (!data || !size || size % 4 || size > 16384)
        throw std::invalid_argument("Invalid instruction signature range");
    std::vector<uint32_t> words(size / 4);
    std::memcpy(words.data(), data, size);
    auto normalized = words;
    for (size_t i = 0; i < words.size(); ++i) {
        const auto word = words[i];
        uint32_t mask = 0;
        if ((word & 0x7c000000) == 0x14000000) {
            const int32_t delta = int32_t(word << 6) >> 4;
            const int64_t target = int64_t(i * 4) + delta;
            if (target < 0 || target >= int64_t(size))
                mask = 0x03ffffff;
        } else if ((word & 0xff000010) == 0x54000000 || (word & 0x7e000000) == 0x34000000) {
            const int32_t delta = int32_t(word << 8) >> 11;
            const int64_t target = int64_t(i * 4) + delta;
            if (target < 0 || target >= int64_t(size))
                mask = 0x00ffffe0;
        } else if ((word & 0x7e000000) == 0x36000000) {
            const int32_t delta = int32_t(word << 13) >> 16;
            const int64_t target = int64_t(i * 4) + delta;
            if (target < 0 || target >= int64_t(size))
                mask = 0x0007ffe0;
        } else if ((word & 0x3b000000) == 0x18000000) {
            const int32_t delta = int32_t(word << 8) >> 11;
            const int64_t target = int64_t(i * 4) + delta;
            if (target < 0 || target >= int64_t(size))
                mask = 0x00ffffe0;
        } else if ((word & 0x9f000000) == 0x10000000) {
            int64_t delta = ((word >> 29) & 3) | (((word >> 5) & 0x7ffff) << 2);
            if (delta & (int64_t(1) << 20))
                delta -= int64_t(1) << 21;
            const int64_t target = int64_t(i * 4) + delta;
            if (target < 0 || target >= int64_t(size))
                mask = 0x60ffffe0;
        } else if ((word & 0x9f000000) == 0x90000000)
            mask = 0x60ffffe0;
        else if ((word & 0xffc00000) == 0x91000000 && (word & 31) == ((word >> 5) & 31)) {
            for (size_t back = 1; back <= 2 && back <= i; ++back) {
                const auto previous = words[i - back];
                if ((previous & 0x9f00001f) == (0x90000000 | (word & 31)) &&
                    (back == 1 || (words[i - 1] & 0x9f000000) == 0x90000000 ||
                     ((words[i - 1] & 0xffc00000) == 0x91000000 &&
                      (words[i - 1] & 31) != (word & 31))))
                    mask = 0x003ffc00;
            }
        }
        normalized[i] &= ~mask;
    }
    unsigned char digest[SHA256_DIGEST_LENGTH];
    SHA256(reinterpret_cast<const unsigned char *>(normalized.data()), size, digest);
    std::string result;
    for (auto byte : digest) {
        result += "0123456789abcdef"[byte >> 4];
        result += "0123456789abcdef"[byte & 15];
    }
    return result;
}
} // namespace ct::steamvr
