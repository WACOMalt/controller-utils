#pragma once
#include "metadata.h"
#include "state.h"
#include <atomic>
#include <functional>
#include <array>

namespace ct::update {
inline constexpr size_t MaxProfile = 16384;
inline constexpr char ProfileRecord[] = "compatibility-profile-v2";
inline constexpr char ProfileBase[] =
    "https://github.com/kallupx/controller-utils/releases/download/compatibility-profiles-v2/";
inline constexpr char ProfileIndex[] =
    "https://api.github.com/repos/kallupx/controller-utils/releases/tags/compatibility-profiles-v2";
std::string profile_asset(json_object *release);
struct Profile {
    uint64_t sequence, expires;
    TrackingBackend backend;
    std::array<std::string, 9> queue_fingerprints;
};
Profile verify_profile_record(const std::string &record, const std::string &trusted_key,
                              double now);
Profile verify_profile(const std::string &bytes, const std::string &signature,
                       const std::string &trusted_key, double now);
std::optional<Profile> load_profile(const ProtectedDirectory &cache, const std::string &trusted_key,
                                    double now);
Profile accept_profile(const MaintenanceLock &lock, const std::string &bytes,
                       const std::string &signature, const std::string &trusted_key, double now);
using ProfileDownload = std::function<void(const std::string &, const std::filesystem::path &,
                                           size_t, const std::atomic_bool &)>;
using ProfileCommit = std::function<void(const std::string &, const std::string &)>;
void acquire_profile(const ProtectedDirectory &cache, const ProfileDownload &download,
                     const std::atomic_bool &cancel, const ProfileCommit &commit);
} // namespace ct::update
