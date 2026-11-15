#pragma once
#include <atomic>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <stdexcept>
#include <string>

namespace ct::update {
class DownloadCancelled : public std::runtime_error {
  public:
    DownloadCancelled() : std::runtime_error("Update download stopped") {}
};
class HttpsDownloader {
    std::filesystem::path certificates_;
    std::function<double()> monotonic_;

  public:
    explicit HttpsDownloader(std::filesystem::path certificates = {},
                             std::function<double()> monotonic = {});
    uint64_t download(const std::string &url, const std::filesystem::path &destination,
                      uint64_t limit, const std::atomic_bool &cancel) const;
};
int download_worker();
} // namespace ct::update
