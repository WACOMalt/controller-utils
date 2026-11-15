#pragma once
#include "installer.h"

namespace ct::update {
struct SetupPolicies {
    std::filesystem::path sudoers = "/etc/sudoers.d/zz-controller-utils-updater";
    std::filesystem::path udev = "/etc/udev/rules.d/70-controller-utils.rules";
    std::filesystem::path service = "/etc/systemd/system/controller-utils-maintenance.service";
    std::filesystem::path keep = "/etc/atomic-update.conf.d/controller-utils.conf";
};
Metadata stage_package(const std::filesystem::path &feed, const std::filesystem::path &destination,
                       const std::string &public_key);
void setup_install(const UpdateInstallation &installation, const SetupPolicies &policies,
                   const std::filesystem::path &feed, const std::string &public_key,
                   const std::function<bool()> &active = runtime_running);
void setup_remove(const UpdateInstallation &installation, const SetupPolicies &policies,
                  const std::string &public_key,
                  const std::function<bool()> &active = runtime_running);
} // namespace ct::update
