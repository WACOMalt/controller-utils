#include "state.h"
#include "platform/linux/files.h"
#include <algorithm>
#include <cerrno>
#include <cmath>
#include <system_error>
#include <fcntl.h>
#include <sys/file.h>

namespace ct::update {
namespace {
void check(int value, const char *operation) {
    if (value < 0)
        throw std::system_error(errno, std::generic_category(), operation);
}
size_t characters(const std::string &value) {
    return size_t(std::count_if(value.begin(), value.end(),
                                [](unsigned char c) { return (c & 0xc0) != 0x80; }));
}
std::filesystem::path prepare_cache(const std::filesystem::path &data) {
    ProtectedDirectory directory(data, getuid());
    if (mkdirat(directory.descriptor(), "updates", 0700) && errno != EEXIST)
        check(-1, "create native update cache");
    check(fsync(directory.descriptor()), "flush native update cache");
    directory.verify();
    return data / "updates";
}
} // namespace
History History::from_json(json_object *value) {
    if (!value || json_object_get_type(value) != json_type_object)
        throw std::invalid_argument("Invalid update history");
    History result;
    for (const auto &[name, destination] : std::array<std::pair<const char *, uint64_t *>, 4>{
             {{"highest_sequence", &result.highest_sequence},
              {"failed_sequence", &result.failed_sequence},
              {"manual_sequence", &result.manual_sequence},
              {"downloaded_sequence", &result.downloaded_sequence}}}) {
        json_object *field = nullptr;
        if (json_object_object_get_ex(value, name, &field)) {
            const auto count = json::unsigned_integer(field);
            if (count > uint64_t(INT64_MAX))
                throw std::invalid_argument("Invalid update history sequence");
            *destination = count;
        }
    }
    json_object *field = nullptr;
    if (json_object_object_get_ex(value, "last_check", &field)) {
        result.last_check = json::number(field);
        if (result.last_check < 0)
            throw std::invalid_argument("Invalid update history time");
    }
    if (json_object_object_get_ex(value, "message", &field)) {
        result.message = json::string(field);
        if (characters(*result.message) > 240 || result.message->find('\0') != std::string::npos)
            throw std::invalid_argument("Invalid update history message");
    }
    return result;
}
json::Value History::to_json() const {
    if (message && (message->size() > 960 || characters(*message) > 240 ||
                    message->find('\0') != std::string::npos))
        throw std::invalid_argument("Invalid update history message");
    json::Value result(json_object_new_object());
    for (const auto &[name, value] : std::array<std::pair<const char *, uint64_t>, 4>{
             {{"highest_sequence", highest_sequence},
              {"failed_sequence", failed_sequence},
              {"manual_sequence", manual_sequence},
              {"downloaded_sequence", downloaded_sequence}}})
        json_object_object_add(result.get(), name, json_object_new_uint64(value));
    json_object_object_add(result.get(), "last_check", json_object_new_double(last_check));
    if (message)
        json_object_object_add(result.get(), "message",
                               json_object_new_string_len(message->data(), int(message->size())));
    History::from_json(result.get());
    return result;
}
History read_history(const ProtectedDirectory &cache) {
    const auto bytes = read_record(cache, "state.json");
    if (!bytes)
        return {};
    auto value = json::parse(*bytes, MaxMetadata);
    return History::from_json(value.get());
}
void save_history(const ProtectedDirectory &cache, const History &history) {
    auto value = history.to_json();
    const auto bytes = json::encode(value.get(), MaxMetadata);
    json::parse(bytes, MaxMetadata);
    write_record(cache, "state.json", bytes);
}
MaintenanceLock::MaintenanceLock(const std::filesystem::path &data)
    : cache_(prepare_cache(data), getuid()) {
    lock_.reset(openat(cache_.descriptor(), "maintenance.lock",
                       O_RDWR | O_CREAT | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC, 0600));
    if (!lock_)
        check(-1, "open update maintenance lock");
    cache_.verify_file("maintenance.lock", lock_.get());
    check(flock(lock_.get(), LOCK_EX | LOCK_NB), "acquire update maintenance lock");
    verify();
}
void MaintenanceLock::verify() const {
    cache_.verify_file("maintenance.lock", lock_.get());
}
} // namespace ct::update
