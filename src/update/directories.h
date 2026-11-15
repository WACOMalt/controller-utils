#pragma once
#include "platform/json.h"
#include "platform/linux/protected_directory.h"

namespace ct::update {
struct DirectoryIdentity {
    uint64_t device, inode;
    bool operator==(const DirectoryIdentity &other) const {
        return device == other.device && inode == other.inode;
    }
    bool operator!=(const DirectoryIdentity &other) const {
        return !(*this == other);
    }
    json::Value to_json() const {
        json::Value value(json_object_new_array_ext(2));
        json_object_array_add(value.get(), json_object_new_uint64(device));
        json_object_array_add(value.get(), json_object_new_uint64(inode));
        return value;
    }
    static DirectoryIdentity from_json(json_object *value) {
        if (json::array_size(value) != 2)
            throw std::invalid_argument("Invalid update directory identity");
        DirectoryIdentity result{json::unsigned_integer(json::item(value, 0)),
                                 json::unsigned_integer(json::item(value, 1))};
        if (!result.inode)
            throw std::invalid_argument("Invalid update directory identity");
        return result;
    }
};
std::optional<DirectoryIdentity> directory_identity(const ProtectedDirectory &parent,
                                                    const std::string &name);
void remove_directory(const ProtectedDirectory &parent, const std::string &name,
                      std::optional<DirectoryIdentity> expected = std::nullopt);
void move_directory(const ProtectedDirectory &from, const std::string &source,
                    const ProtectedDirectory &to, const std::string &destination, unsigned flags);
} // namespace ct::update
