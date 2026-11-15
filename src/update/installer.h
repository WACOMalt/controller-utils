#pragma once
#include "transaction.h"

namespace ct::update {
struct UpdateInstallation {
    Installation components;
    std::filesystem::path data, cache;
    uid_t user;
    gid_t group;
};
bool runtime_running();
void install_update(const UpdateInstallation &installation, bool confirmation,
                    const std::function<bool()> &active = runtime_running);
} // namespace ct::update
