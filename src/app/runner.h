#pragma once
#include "host.h"
#include "platform/linux/signals.h"

namespace ct {
int run_application(NativeApplicationHost &host, Runtime &runtime, ControllerReader &reader,
                    SignalQueue &signals, const std::filesystem::path &data, bool background);
} // namespace ct
