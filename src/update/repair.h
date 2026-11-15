#pragma once
#include "installer.h"
#include "platform/linux/child.h"
#include "setup.h"

namespace ct::update {
void record_repair_evidence(const ProtectedDirectory &native, const std::string &metadata,
                            const std::string &signature, const std::string &trusted_key,
                            const Release &expected);
Release verify_repair_updater(const ProtectedDirectory &native, const std::string &trusted_key);
Metadata verify_native_evidence(const ProtectedDirectory &native, const std::string &trusted_key);
enum class RepairStatus {
    NeedsAuthorization,
    Busy,
    Intact,
    Missing,
    Conflict,
    ReinstallRequired,
    Interrupted
};
RepairStatus inspect_repair(const UpdateInstallation &installation, const SetupPolicies &policies,
                            const std::string &trusted_key);
void repair_permissions(const UpdateInstallation &installation, const SetupPolicies &policies,
                        const std::string &trusted_key, const std::function<void()> &reload);
std::unique_ptr<ChildChannel> start_repair_action(const ChildFactory &children,
                                                  const std::function<void()> &verify);
std::unique_ptr<ChildChannel> start_inspection_action(const ChildFactory &children,
                                                      const std::function<void()> &verify);
} // namespace ct::update
