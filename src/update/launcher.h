#pragma once
#include <filesystem>
#include <string>
#include <vector>

namespace ct::update {
int launch(const std::filesystem::path &application, const std::filesystem::path &data,
           const std::vector<std::string> &arguments);
} // namespace ct::update
