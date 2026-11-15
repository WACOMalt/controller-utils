#include "profiles.h"
#include <algorithm>
#include <cmath>
#include <charconv>
#include <set>

namespace ct::update {
namespace {
void fields(json_object *value, std::initializer_list<const char *> names) {
    if (!value || json_object_get_type(value) != json_type_object ||
        json_object_object_length(value) != int(names.size()))
        throw std::invalid_argument("Invalid compatibility profile fields");
    for (const auto name : names)
        (void)json::field(value, name);
}
bool digest(const std::string &value) {
    return value.size() == 64 && std::all_of(value.begin(), value.end(), [](char c) {
               return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
           });
}
} // namespace
std::string profile_asset(json_object *release) {
    auto assets = json::field(release, "assets");
    if (json::array_size(assets) > 128)
        throw std::invalid_argument("Compatibility release has too many assets");
    uint64_t highest = 0;
    std::string selected;
    std::set<uint64_t> sequences;
    const std::string prefix = "compatibility-profile-", suffix = ".bin";
    for (size_t i = 0; i < json::array_size(assets); ++i) {
        const auto item = json::item(assets, i);
        const auto name = json::string(json::field(item, "name"));
        if (name.size() <= prefix.size() + suffix.size() ||
            name.compare(0, prefix.size(), prefix) ||
            name.compare(name.size() - suffix.size(), suffix.size(), suffix))
            continue;
        const auto digits = name.substr(prefix.size(), name.size() - prefix.size() - suffix.size());
        uint64_t sequence = 0;
        const auto parsed = std::from_chars(digits.data(), digits.data() + digits.size(), sequence);
        if (parsed.ec != std::errc{} || parsed.ptr != digits.data() + digits.size() || !sequence ||
            sequence > uint64_t(INT64_MAX) || std::to_string(sequence) != digits)
            throw std::invalid_argument("Invalid compatibility asset sequence");
        if (json::string(json::field(item, "state")) != "uploaded")
            continue;
        const auto size = json::unsigned_integer(json::field(item, "size"));
        if (size <= 64 || size > MaxProfile + 64 || !sequences.insert(sequence).second)
            throw std::invalid_argument("Invalid or duplicate compatibility asset");
        if (sequence > highest) {
            highest = sequence;
            selected = name;
        }
    }
    if (selected.empty())
        throw std::runtime_error("No completed compatibility profile is published");
    return selected;
}
Profile verify_profile_record(const std::string &record, const std::string &key, double now) {
    if (record.size() <= 64 || record.size() > MaxProfile + 64)
        throw std::invalid_argument("Invalid stored compatibility profile");
    return verify_profile(record.substr(64), record.substr(0, 64), key, now);
}
Profile verify_profile(const std::string &bytes, const std::string &signature,
                       const std::string &trusted_key, double now) {
    if (bytes.empty() || bytes.size() > MaxProfile || !std::isfinite(now) || now < 0)
        throw std::invalid_argument("Invalid compatibility profile size or time");
    verify_signed_bytes(bytes, signature, trusted_key);
    auto value = json::parse(bytes, MaxProfile);
    if (json::encode(value.get(), MaxProfile) != bytes)
        throw std::invalid_argument("Compatibility profile is not canonical JSON");
    fields(value.get(),
           {"schema", "sequence", "expires", "platform", "validator", "backend", "queue"});
    if (json::integer(json::field(value.get(), "schema")) != 2 ||
        json::string(json::field(value.get(), "platform")) != "linuxarm64" ||
        json::string(json::field(value.get(), "validator")) != "queue-v2")
        throw std::invalid_argument("Unsupported compatibility profile contract");
    const auto sequence = json::unsigned_integer(json::field(value.get(), "sequence"));
    const auto expires = json::unsigned_integer(json::field(value.get(), "expires"));
    if (!sequence || sequence > uint64_t(INT64_MAX) || !expires || expires > uint64_t(INT64_MAX) ||
        double(expires) <= now)
        throw std::invalid_argument("Expired or invalid compatibility profile");
    auto queue = json::field(value.get(), "queue");
    fields(queue, {"fingerprints"});
    Profile result{sequence,
                   expires,
                   backend_from_name(json::string(json::field(value.get(), "backend"))),
                   {}};
    auto fingerprints = json::field(queue, "fingerprints");
    if (json::array_size(fingerprints) != result.queue_fingerprints.size())
        throw std::invalid_argument("Invalid compatibility profile identity");
    for (size_t i = 0; i < result.queue_fingerprints.size(); ++i) {
        result.queue_fingerprints[i] = json::string(json::item(fingerprints, i));
        if (!digest(result.queue_fingerprints[i]))
            throw std::invalid_argument("Invalid compatibility profile fingerprint");
    }
    return result;
}
std::optional<Profile> load_profile(const ProtectedDirectory &cache, const std::string &trusted_key,
                                    double now) {
    const auto record = read_record(cache, ProfileRecord, MaxProfile + 64);
    if (!record)
        return std::nullopt;
    return verify_profile_record(*record, trusted_key, now);
}
} // namespace ct::update
