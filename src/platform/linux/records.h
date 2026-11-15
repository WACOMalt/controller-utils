#pragma once
#include "protected_directory.h"
#include <optional>

namespace ct {
inline constexpr size_t MaxRecordSize = 256 * 1024 * 1024;
std::optional<std::string> read_record(const ProtectedDirectory &directory, const std::string &name,
                                       size_t limit = 65536);
void write_record(const ProtectedDirectory &directory, const std::string &name,
                  const std::string &bytes, uint32_t mode = 0600);
void erase_record(const ProtectedDirectory &directory, const std::string &name);
} // namespace ct
