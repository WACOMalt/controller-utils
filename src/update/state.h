#pragma once
#include "metadata.h"
#include "platform/linux/records.h"

namespace ct::update {
struct History {
    uint64_t highest_sequence = 0, failed_sequence = 0, manual_sequence = 0,
             downloaded_sequence = 0;
    double last_check = 0;
    std::optional<std::string> message;
    static History from_json(json_object *value);
    json::Value to_json() const;
};
History read_history(const ProtectedDirectory &cache);
void save_history(const ProtectedDirectory &cache, const History &history);
class MaintenanceLock {
    ProtectedDirectory cache_;
    FileDescriptor lock_;

  public:
    explicit MaintenanceLock(const std::filesystem::path &data);
    MaintenanceLock(const MaintenanceLock &) = delete;
    MaintenanceLock &operator=(const MaintenanceLock &) = delete;
    const ProtectedDirectory &cache() const {
        return cache_;
    }
    void verify() const;
};
} // namespace ct::update
