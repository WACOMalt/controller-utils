#include "metadata.h"
#include "platform/linux/files.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <curl/curl.h>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string_view>
#include <vector>

namespace ct::update {
namespace {
void fields(json_object *value, std::initializer_list<const char *> names, const char *error) {
    if (!value || json_object_get_type(value) != json_type_object ||
        json_object_object_length(value) != int(names.size()))
        throw std::invalid_argument(error);
    for (auto name : names) {
        json_object *field = nullptr;
        if (!json_object_object_get_ex(value, name, &field))
            throw std::invalid_argument(error);
    }
}
bool version(const std::string &text) {
    unsigned dots = 0, digits = 0;
    for (const auto c : text) {
        if (c == '.') {
            if (!digits || ++dots > 2)
                return false;
            digits = 0;
        } else if (c < '0' || c > '9' || ++digits > 6)
            return false;
    }
    return dots == 2 && digits;
}
bool digest(const std::string &text) {
    return text.size() == 64 && std::all_of(text.begin(), text.end(), [](char c) {
               return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
           });
}
size_t characters(const std::string &text) {
    return size_t(std::count_if(text.begin(), text.end(),
                                [](unsigned char c) { return (c & 0xc0) != 0x80; }));
}
void fingerprints(json_object *value, const std::vector<std::string> &names) {
    if (!value || json_object_get_type(value) != json_type_object ||
        json_object_object_length(value) != int(names.size()))
        throw std::invalid_argument("Invalid update compatibility fingerprints");
    for (const auto &name : names)
        if (!digest(json::string(json::field(value, name.c_str()))))
            throw std::invalid_argument("Invalid update compatibility fingerprints");
}
uint64_t sequence(json_object *value) {
    const auto result = json::unsigned_integer(value);
    if (!result || result > uint64_t(INT64_MAX))
        throw std::invalid_argument("Invalid installed release information");
    return result;
}
Release release_fields(json_object *value) {
    auto text = json::string(json::field(value, "version"));
    if (!version(text) || json::string(json::field(value, "platform")) != "linuxarm64")
        throw std::invalid_argument("Invalid installed release information");
    return {std::move(text), sequence(json::field(value, "sequence"))};
}
} // namespace

std::vector<std::string> helper_files(TrackingBackend backend) {
    backend_name(backend);
    std::vector<std::string> result{"controller-utils-helper", "controller-utils-updater"};
    for (const auto name : {"adapter.json", "loader.bin", "libnative_tracker.so", "manifest.json"})
        result.push_back("native-tracker/" + std::string(name));
    return result;
}
void validate_requirements(json_object *value) {
    fields(value, {"backend", "driver", "helper", "runtime"},
           "Invalid update compatibility requirements");
    const auto backend = backend_from_name(json::string(json::field(value, "backend")));
    fingerprints(json::field(value, "driver"), {DriverFiles.begin(), DriverFiles.end()});
    fingerprints(json::field(value, "helper"), helper_files(backend));
    fingerprints(json::field(value, "runtime"), {RuntimeFiles.begin(), RuntimeFiles.end()});
}
Release Release::from_json(json_object *value) {
    fields(value, {"version", "sequence", "platform"}, "Invalid installed release information");
    return release_fields(value);
}
json::Value Release::to_json() const {
    json::Value value(json_object_new_object());
    json_object_object_add(value.get(), "version", json_object_new_string(version.c_str()));
    json_object_object_add(value.get(), "sequence", json_object_new_uint64(sequence));
    json_object_object_add(value.get(), "platform", json_object_new_string("linuxarm64"));
    return value;
}
bool Release::operator==(const Release &other) const {
    return sequence == other.sequence && version == other.version;
}
Release release(const std::filesystem::path &root) {
    auto value = json::parse(read_file(root / "release.json", MaxMetadata), MaxMetadata);
    return Release::from_json(value.get());
}
std::string release_url(const std::string &channel, const Release &version) {
    return channel == GitHubChannel
               ? std::string(GitHubReleases) + "download/v" + version.version + "/"
               : channel;
}
std::string https_url(const std::string &url) {
    if (url.empty() ||
        std::any_of(url.begin(), url.end(), [](unsigned char c) { return c <= 32 || c == 127; }))
        throw std::invalid_argument("Updates require a fixed HTTPS directory");
    std::unique_ptr<CURLU, decltype(&curl_url_cleanup)> parsed(curl_url(), curl_url_cleanup);
    if (!parsed)
        throw std::bad_alloc();
    if (curl_url_set(parsed.get(), CURLUPART_URL, url.c_str(), CURLU_DISALLOW_USER))
        throw std::invalid_argument("Updates require a fixed HTTPS directory");
    auto part = [&](CURLUPart item) -> std::optional<std::string> {
        char *text = nullptr;
        const auto result = curl_url_get(parsed.get(), item, &text, 0);
        std::unique_ptr<char, decltype(&curl_free)> owned(text, curl_free);
        if (result)
            return std::nullopt;
        return std::string(text);
    };
    const auto host = part(CURLUPART_HOST);
    if (part(CURLUPART_SCHEME) != "https" || !host || host->empty() || part(CURLUPART_QUERY) ||
        part(CURLUPART_FRAGMENT))
        throw std::invalid_argument("Updates require a fixed HTTPS directory");
    return url;
}
std::string fixed_https_directory(const std::string &url) {
    if (url.empty() || url.back() != '/')
        throw std::invalid_argument("Updates require a fixed HTTPS directory");
    return https_url(url);
}
std::optional<std::string> channel(const std::filesystem::path &root) {
    const auto path = root / "assets/update-channel.json";
    if (!std::filesystem::exists(path))
        return std::nullopt;
    auto value = json::parse(read_file(path, MaxMetadata), MaxMetadata);
    fields(value.get(), {"url"}, "Invalid update channel");
    auto url = fixed_https_directory(json::string(json::field(value.get(), "url")));
    if (!std::filesystem::is_regular_file(root / "assets/update-public.pem"))
        throw std::invalid_argument("Update signing key is missing");
    return url;
}
Metadata Metadata::from_json(json_object *value, uint64_t minimum, double now) {
    fields(value,
           {"schema", "version", "sequence", "platform", "expires", "artifact", "size", "sha256",
            "requirements", "notes"},
           "Invalid update metadata");
    if (json::integer(json::field(value, "schema")) != 5)
        throw std::invalid_argument(
            "This release needs a native compatibility update before it can install");
    auto installed = release_fields(value);
    const auto expires = json::unsigned_integer(json::field(value, "expires"));
    const auto size = json::unsigned_integer(json::field(value, "size"));
    const auto artifact = json::string(json::field(value, "artifact"));
    const auto sha = json::string(json::field(value, "sha256"));
    const auto notes = json::string(json::field(value, "notes"));
    constexpr std::string_view prefix = "controller-utils-", suffix = "-linuxarm64.tar.gz";
    const bool filename =
        artifact.size() > prefix.size() + suffix.size() &&
        artifact.compare(0, prefix.size(), prefix) == 0 &&
        artifact.compare(artifact.size() - suffix.size(), suffix.size(), suffix) == 0 &&
        std::all_of(artifact.begin(), artifact.end(), [](char c) {
            return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
                   c == '.' || c == '_' || c == '-';
        });
    if (installed.sequence < std::max(uint64_t(1), minimum) || !std::isfinite(now) || now < 0 ||
        expires > uint64_t(INT64_MAX) || (expires && double(expires) <= now) || !size ||
        size > MaxArchive || !filename || !digest(sha) || characters(notes) > 240)
        throw std::invalid_argument("Expired, incompatible or older update metadata");
    auto requirements = json::field(value, "requirements");
    validate_requirements(requirements);
    return {std::move(installed),
            expires,
            size,
            artifact,
            sha,
            notes,
            json::Value(json_object_get(requirements))};
}
Metadata verify_metadata_key(const std::filesystem::path &metadata,
                             const std::filesystem::path &signature, const std::string &public_key,
                             uint64_t minimum, std::optional<double> now) {
    const auto contents = read_file(metadata, MaxMetadata), sig = read_file(signature, 64);
    verify_signed_bytes(contents, sig, public_key);
    auto value = json::parse(contents, MaxMetadata);
    const auto wall =
        std::chrono::duration<double>(std::chrono::system_clock::now().time_since_epoch()).count();
    return Metadata::from_json(value.get(), minimum, now.value_or(wall));
}
Metadata verify_metadata(const std::filesystem::path &metadata,
                         const std::filesystem::path &signature,
                         const std::filesystem::path &public_key, uint64_t minimum,
                         std::optional<double> now) {
    return verify_metadata_key(metadata, signature, read_file(public_key, 16384), minimum, now);
}
} // namespace ct::update
