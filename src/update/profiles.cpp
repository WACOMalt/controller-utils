#include "profiles.h"
#include "directories.h"
#include "download.h"
#include <algorithm>
#include <cmath>
#include <limits>
#include <cerrno>
#include <system_error>
#include <sys/stat.h>
#include <unistd.h>

namespace ct::update {
Profile accept_profile(const MaintenanceLock &lock, const std::string &bytes,
                       const std::string &signature, const std::string &trusted_key, double now) {
    const auto incoming = verify_profile(bytes, signature, trusted_key, now);
    lock.verify();
    const auto old = read_record(lock.cache(), ProfileRecord, MaxProfile + 64);
    const auto record = signature + bytes;
    if (old) {
        const auto current = verify_profile_record(*old, trusted_key, 0);
        if (incoming.sequence < current.sequence ||
            (incoming.sequence == current.sequence && record != *old))
            throw std::invalid_argument("Compatibility profile rollback or substitution");
        if (record == *old)
            return incoming;
    }
    lock.verify();
    write_record(lock.cache(), ProfileRecord, record);
    lock.verify();
    return incoming;
}
void acquire_profile(const ProtectedDirectory &cache, const ProfileDownload &download,
                     const std::atomic_bool &cancel, const ProfileCommit &commit) {
    remove_directory(cache, "profile-download");
    if (mkdirat(cache.descriptor(), "profile-download", 0700))
        throw std::system_error(errno, std::generic_category(), "create profile download stage");
    try {
        ProtectedDirectory work(cache.path() / "profile-download", getuid());
        download(ProfileIndex, work.path() / "release.json", MaxMetadata, cancel);
        const auto index = read_record(work, "release.json", MaxMetadata);
        if (!index)
            throw std::runtime_error("Compatibility index download is incomplete");
        const auto asset = profile_asset(json::parse(*index).get());
        download(std::string(ProfileBase) + asset, work.path() / "profile.bin", MaxProfile + 64,
                 cancel);
        const auto record = read_record(work, "profile.bin", MaxProfile + 64);
        if (!record || record->size() <= 64)
            throw std::runtime_error("Compatibility profile download is incomplete");
        const auto body = record->substr(64);
        const auto value = json::parse(body, MaxProfile);
        const auto sequence = json::unsigned_integer(json::field(value.get(), "sequence"));
        if (asset != "compatibility-profile-" + std::to_string(sequence) + ".bin")
            throw std::runtime_error("Compatibility asset sequence mismatch");
        if (cancel.load())
            throw DownloadCancelled();
        commit(body, record->substr(0, 64));
    } catch (...) {
        remove_directory(cache, "profile-download");
        throw;
    }
    remove_directory(cache, "profile-download");
}
} // namespace ct::update
