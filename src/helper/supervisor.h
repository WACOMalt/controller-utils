#pragma once
#include "backend.h"
#include "recovery.h"

namespace ct::helper {
class Supervisor {
    TrackingBackend &backend_;
    RecoveryGuard &guard_;
    RecoveryOutput output_;
    int input_;

  public:
    Supervisor(TrackingBackend &backend, RecoveryGuard &guard, int input = 0, int output = 1);
    void run();
};
} // namespace ct::helper
