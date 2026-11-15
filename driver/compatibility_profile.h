#pragma once
#include "steamvr/queue_abi.h"
#include "native_queue_startup.h"
#include "update/profiles.h"

namespace ct::queue_startup {
inline bool validate_profile_queue(const uintptr_t *table, const ct::steamvr::Module &module,
                                   const ct::update::Profile &profile,
                                   ct::TrackingBackend backend) {
    if (profile.backend != backend)
        return false;
    auto layout = ct::steamvr::ServerQueueLayout;
    for (size_t i = 0; i < profile.queue_fingerprints.size(); ++i)
        layout.methods[i].signature = profile.queue_fingerprints[i].c_str();
    return ct::steamvr::validate_queue_layout(table, module, layout);
}
bool initialize_compatible(ct::steamvr::BlockQueue *queue);
} // namespace ct::queue_startup
