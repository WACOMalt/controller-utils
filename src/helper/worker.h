#pragma once
#include "patch.h"
#include <functional>

namespace ct::helper {
void require_helper_process();
enum class PatchResult { Applied, Reused, Busy, Restored };
PatchResult run_patch_worker(PinnedProcess &process,
                             const std::function<PatchResult(StoppedProcess &)> &operation,
                             double timeout = 1.5);
void patch_gate(PinnedProcess &process, const GateLayout &layout, bool apply);
} // namespace ct::helper
