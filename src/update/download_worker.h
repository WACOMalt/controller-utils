#pragma once
#include <cstdint>
#include <filesystem>
#include <string>

namespace ct::update {
uint64_t download_to_descriptor(const std::string &url, int output, uint64_t limit,
                                const std::filesystem::path &certificates);
} // namespace ct::update
