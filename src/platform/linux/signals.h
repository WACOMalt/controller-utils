#pragma once
#include "resource.h"
#include <initializer_list>
#include <optional>
#include <signal.h>

namespace ct {
class SignalQueue {
    sigset_t previous_{};
    FileDescriptor events_;

  public:
    explicit SignalQueue(std::initializer_list<int> signals);
    SignalQueue(const SignalQueue &) = delete;
    SignalQueue &operator=(const SignalQueue &) = delete;
    ~SignalQueue();
    int descriptor() const {
        return events_.get();
    }
    std::optional<int> receive();
};
} // namespace ct
