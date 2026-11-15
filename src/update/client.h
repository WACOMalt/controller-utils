#pragma once
#include "compatibility.h"
#include "download.h"
#include "profiles.h"
#include "state.h"
#include <mutex>
#include <thread>

namespace ct::update {
struct UpdateStatus {
    std::string version, message;
    bool automatic, configured, busy;
    json::Value to_json() const;
};
class UpdateClient {
    std::filesystem::path application_, data_;
    Release version_;
    std::optional<std::string> url_;
    HttpsDownloader downloader_;
    ProfileDownload transport_;
    std::string profile_key_;
    std::function<InstalledRequirements()> installed_;
    std::function<double()> wall_;
    History history_;
    std::mutex mutex_;
    std::thread worker_, profile_worker_;
    std::atomic_bool cancel_{false}, profile_busy_{false};
    std::string message_;
    double last_check_ = 0;
    bool busy_ = false, closed_ = false;
    void message(std::string value);
    void run(bool manual, double checked) noexcept;
    void fetch(const ProtectedDirectory &cache, bool manual);
    void fetch_profile() noexcept;

  public:
    UpdateClient(std::filesystem::path application, std::filesystem::path data,
                 std::function<InstalledRequirements()> installed,
                 std::filesystem::path certificates = {}, std::function<double()> wall = {},
                 ProfileDownload transport = {}, std::string profile_key = {});
    UpdateClient(const UpdateClient &) = delete;
    UpdateClient &operator=(const UpdateClient &) = delete;
    ~UpdateClient();
    void check(bool manual = false);
    UpdateStatus poll(bool automatic);
    bool profile_busy() const {
        return profile_busy_.load();
    }
    void close();
};
} // namespace ct::update
