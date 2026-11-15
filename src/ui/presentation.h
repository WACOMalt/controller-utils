#pragma once
#include "messages.h"
#include <deque>
#include <mutex>

namespace ct::ui {
class Presentation {
    mutable std::mutex mutex_;
    std::deque<Command> commands_;
    std::deque<Event> events_;
    bool closed_ = false;
    template <class T> void push(std::deque<T> &queue, T value) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (closed_)
            return;
        if (queue.size() == 128) {
            closed_ = true;
            throw std::runtime_error("Controller Utils UI queue overflow");
        }
        queue.push_back(std::move(value));
    }
    template <class T> std::deque<T> drain(std::deque<T> &queue) {
        std::lock_guard<std::mutex> lock(mutex_);
        std::deque<T> result;
        result.swap(queue);
        return result;
    }

  public:
    void send(Command command) {
        push(commands_, std::move(command));
    }
    void emit(Event event) {
        push(events_, std::move(event));
    }
    std::deque<Command> commands() {
        return drain(commands_);
    }
    std::deque<Event> events() {
        return drain(events_);
    }
    bool running() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return !closed_;
    }
    void close() {
        std::lock_guard<std::mutex> lock(mutex_);
        closed_ = true;
    }
};
} // namespace ct::ui
