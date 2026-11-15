#pragma once
#include "app/instance.h"
#include "compatibility.h"
#include "state.h"
#include <functional>

namespace ct::update {
class Transaction {
    std::filesystem::path application_;
    ProtectedDirectory parent_, data_;
    const ApplicationLock &application_lock_;
    const MaintenanceLock &maintenance_;
    std::function<InstalledRequirements()> installed_;
    void verify() const;

  public:
    Transaction(std::filesystem::path application, const std::filesystem::path &data,
                const ApplicationLock &application_lock, const MaintenanceLock &maintenance,
                std::function<InstalledRequirements()> installed);
    void recover(bool force = false);
    bool activate();
};
void confirm(const std::filesystem::path &application, const std::filesystem::path &data,
             const ApplicationLock &application_lock);
} // namespace ct::update
