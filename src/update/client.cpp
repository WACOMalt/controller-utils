#include "client.h"
#include "directories.h"
#include "platform/linux/files.h"
#include "update_public_key.h"
#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <iostream>
#include <system_error>
#include <linux/fs.h>

namespace ct::update {
namespace {
double wall_time() {
    return std::chrono::duration<double>(std::chrono::system_clock::now().time_since_epoch())
        .count();
}
std::string text(const std::string &value) {
    size_t length = 0, count = 0;
    while (length < value.size()) {
        if ((static_cast<unsigned char>(value[length]) & 0xc0) != 0x80 && ++count > 240)
            break;
        ++length;
    }
    return value.substr(0, length);
}
} // namespace
json::Value UpdateStatus::to_json() const {
    json::Value value(json_object_new_object());
    json_object_object_add(value.get(), "op", json_object_new_string("updates"));
    json_object_object_add(value.get(), "version", json_object_new_string(version.c_str()));
    json_object_object_add(value.get(), "message", json_object_new_string(message.c_str()));
    json_object_object_add(value.get(), "automatic", json_object_new_boolean(automatic));
    json_object_object_add(value.get(), "configured", json_object_new_boolean(configured));
    json_object_object_add(value.get(), "busy", json_object_new_boolean(busy));
    return value;
}
UpdateClient::UpdateClient(std::filesystem::path application, std::filesystem::path data,
                           std::function<InstalledRequirements()> installed,
                           std::filesystem::path certificates, std::function<double()> wall,
                           ProfileDownload transport, std::string profile_key,
                           std::function<SteamOsChannel()> os_channel)
    : application_(std::move(application)), data_(std::move(data)), version_(release(application_)),
      downloader_(std::move(certificates)),
      transport_(transport ? std::move(transport)
                           : ProfileDownload([this](const std::string &url,
                                                    const std::filesystem::path &path, size_t limit,
                                                    const std::atomic_bool &cancel) {
                                 downloader_.download(url, path, limit, cancel);
                             })),
      profile_key_(profile_key.empty() ? UpdatePublicKey : std::move(profile_key)),
      installed_(std::move(installed)), wall_(wall ? std::move(wall) : wall_time),
      os_channel_(os_channel ? std::move(os_channel) : steam_os_channel) {
    if (!installed_)
        throw std::invalid_argument("Missing installed update compatibility capture");
    std::optional<std::string> error;
    try {
        url_ = channel(application_);
    } catch (const std::exception &failure) {
        error = failure.what();
    }
    try {
        ProtectedDirectory directory(data_, getuid());
        if (directory_identity(directory, "updates")) {
            ProtectedDirectory cache(data_ / "updates", getuid());
            history_ = read_history(cache);
        }
    } catch (const std::exception &failure) {
        url_.reset();
        error = "Update history could not be read. " + std::string(failure.what());
    }
    message_ = url_ ? history_.message.value_or("Ready to check for updates.")
                    : error.value_or("Update server is not configured.");
    last_check_ = history_.last_check;
}
UpdateClient::~UpdateClient() {
    close();
}
void UpdateClient::message(std::string value) {
    std::lock_guard lock(mutex_);
    message_ = text(value);
}
void UpdateClient::check(bool manual) {
    std::lock_guard lock(mutex_);
    if (closed_ || busy_ || !url_)
        return;
    if (worker_.joinable())
        worker_.join();
    const auto checked = wall_();
    if (!std::isfinite(checked) || checked < 0)
        throw std::runtime_error("Invalid update check time");
    try {
        busy_ = true;
        worker_ = std::thread([this, manual, checked] { run(manual, checked); });
        last_check_ = checked;
        message_ = "Checking for updates...";
    } catch (const std::exception &failure) {
        busy_ = false;
        last_check_ = checked;
        message_ = "Couldn't check for updates. Try again later.";
        std::cerr << "Controller Utils update: " << failure.what() << '\n';
    }
}
void UpdateClient::fetch(const ProtectedDirectory &cache, bool manual) {
    remove_directory(cache, "download");
    if (mkdirat(cache.descriptor(), "download", 0700))
        throw std::system_error(errno, std::generic_category(), "create update download stage");
    ProtectedDirectory work(cache.path() / "download", getuid());
    const bool stable = *url_ == GitHubChannel && os_channel_() == SteamOsChannel::Stable;
    try {
        transport_(*url_ + (stable ? "latest-stable.json" : "latest.json"),
                   work.path() / "latest.json", MaxMetadata, cancel_);
    } catch (const std::exception &error) {
        if (std::string(error.what()).find("HTTP 404") == std::string::npos)
            throw;
        message("No releases have been published yet.");
        return;
    }
    auto untrusted = json::parse(read_file(work.path() / "latest.json", MaxMetadata), MaxMetadata);
    auto version = json::Value(json_object_new_object());
    for (const auto field : {"version", "sequence", "platform"})
        json_object_object_add(version.get(), field,
                               json_object_get(json::field(untrusted.get(), field)));
    const auto pinned = release_url(*url_, Release::from_json(version.get()));
    transport_(pinned + (stable ? "latest-stable.sig" : "latest.sig"), work.path() / "latest.sig",
               64, cancel_);
    const auto metadata =
        verify_metadata(work.path() / "latest.json", work.path() / "latest.sig",
                        application_ / "assets/update-public.pem",
                        std::max(version_.sequence, history_.highest_sequence), wall_());
    history_.highest_sequence = metadata.release.sequence;
    if (metadata.release.sequence <= version_.sequence) {
        message("You're up to date.");
        return;
    }
    if (history_.failed_sequence == metadata.release.sequence && !manual) {
        message("The previous version was restored. Choose Check now to retry.");
        return;
    }
    check_requirements(metadata.requirements.get(), installed_());
    message("Downloading version " + metadata.release.version + "...");
    const auto path = work.path() / metadata.artifact;
    transport_(release_url(*url_, metadata.release) + metadata.artifact, path, metadata.size,
               cancel_);
    const auto file = work.file(metadata.artifact, MaxArchive);
    struct stat image{};
    if (fstat(file.get(), &image))
        throw std::system_error(errno, std::generic_category(), "inspect downloaded update");
    if (uint64_t(image.st_size) != metadata.size || sha256(file.get()) != metadata.sha256)
        throw std::runtime_error("Downloaded update checksum mismatch");
    work.verify_file(metadata.artifact, file.get());
    if (cancel_.load())
        throw DownloadCancelled();
    const auto previous = directory_identity(cache, "pending");
    const auto downloaded = *directory_identity(cache, "download");
    move_directory(cache, "download", cache, "pending",
                   previous ? RENAME_EXCHANGE : RENAME_NOREPLACE);
    if (previous)
        remove_directory(cache, "download", *previous);
    if (directory_identity(cache, "pending") != downloaded)
        throw std::runtime_error("Pending update identity changed during download");
    if (manual)
        history_.manual_sequence = metadata.release.sequence;
    history_.downloaded_sequence = metadata.release.sequence;
    message("Version " + metadata.release.version +
            " is ready. It will install when SteamVR next starts.");
}
void UpdateClient::fetch_profile() noexcept {
    try {
        ProtectedDirectory cache(data_ / "updates", getuid());
        acquire_profile(cache, transport_, cancel_,
                        [this](const std::string &bytes, const std::string &signature) {
                            MaintenanceLock lock(data_);
                            if (cancel_.load())
                                throw DownloadCancelled();
                            accept_profile(lock, bytes, signature, profile_key_, wall_());
                        });
    } catch (const DownloadCancelled &) {
    } catch (const std::exception &failure) {
        std::cerr << "Controller Utils compatibility profile: " << failure.what() << '\n';
    }
    profile_busy_ = false;
}
void UpdateClient::run(bool manual, double checked) noexcept {
    try {
        MaintenanceLock lease(data_);
        const auto highest = history_.highest_sequence;
        history_ = read_history(lease.cache());
        history_.highest_sequence = std::max(highest, history_.highest_sequence);
        try {
            fetch(lease.cache(), manual);
        } catch (const DownloadCancelled &) {
            message(history_.message.value_or("Ready to check for updates."));
        } catch (const std::exception &failure) {
            const auto reason = std::string(failure.what());
            message(reason.find("native compatibility update") != std::string::npos
                        ? "This release needs a native compatibility update."
                        : "Couldn't check for updates. Try again later.");
            std::cerr << "Controller Utils update: " << reason << '\n';
        }
        remove_directory(lease.cache(), "download");
        history_.last_check = checked;
        {
            std::lock_guard lock(mutex_);
            history_.message = message_;
        }
        save_history(lease.cache(), history_);
    } catch (const std::exception &failure) {
        message("Couldn't check for updates. Try again later.");
        std::cerr << "Controller Utils update state: " << failure.what() << '\n';
    }
    std::lock_guard lock(mutex_);
    if (!closed_ && !cancel_.load() && !profile_busy_.exchange(true)) {
        if (profile_worker_.joinable())
            profile_worker_.join();
        try {
            profile_worker_ = std::thread([this] { fetch_profile(); });
        } catch (const std::exception &failure) {
            profile_busy_ = false;
            std::cerr << "Controller Utils compatibility profile: " << failure.what() << '\n';
        }
    }
    busy_ = false;
}
UpdateStatus UpdateClient::poll(bool automatic) {
    bool due;
    {
        std::lock_guard lock(mutex_);
        const auto elapsed = wall_() - last_check_;
        due = automatic && url_ && (elapsed < 0 || elapsed >= 12 * 60 * 60);
    }
    if (due)
        check();
    std::lock_guard lock(mutex_);
    return {version_.version, text(message_), automatic, bool(url_), busy_};
}
void UpdateClient::close() {
    std::thread worker, profile_worker;
    {
        std::lock_guard lock(mutex_);
        closed_ = true;
        cancel_ = true;
        worker = std::move(worker_);
        profile_worker = std::move(profile_worker_);
    }
    if (worker.joinable())
        worker.join();
    if (profile_worker.joinable())
        profile_worker.join();
}
} // namespace ct::update
