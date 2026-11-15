#include "backend.h"
#include <stdexcept>
#include <system_error>

namespace ct {
TrackingBackend installed_backend(const std::filesystem::path &helpers) {
    if (!helpers.is_absolute() || helpers.lexically_normal() != helpers ||
        helpers.string().find('\0') != std::string::npos)
        throw std::invalid_argument("Invalid installed tracking backend path");
    std::error_code error;
    const auto status = std::filesystem::symlink_status(helpers / "native-tracker", error);
    if (error && error != std::errc::no_such_file_or_directory)
        throw std::system_error(error, "inspect installed tracking backend");
    if (error || status.type() == std::filesystem::file_type::not_found)
        throw std::runtime_error("Native tracker is not installed");
    if (!std::filesystem::is_directory(status))
        throw std::runtime_error("Invalid installed native tracker directory");
    return TrackingBackend::Tracker;
}
} // namespace ct
