#pragma once
#include <cstdint>
#include <filesystem>
#include <string>

namespace ct {
std::string read_file(const std::filesystem::path &path, size_t limit);
std::string read_file(int descriptor, size_t limit);
std::string sha256_bytes(const void *data, size_t size);
void atomic_write(const std::filesystem::path &path, const std::string &contents,
                  uint32_t mode = 0600);
} // namespace ct
