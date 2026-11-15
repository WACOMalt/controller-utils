#pragma once
#include <cstdint>
#include <utility>
#include <unistd.h>

namespace ct {
class FileDescriptor {
    int value_ = -1;

  public:
    FileDescriptor() = default;
    explicit FileDescriptor(int value) : value_(value) {}
    FileDescriptor(const FileDescriptor &) = delete;
    FileDescriptor &operator=(const FileDescriptor &) = delete;
    FileDescriptor(FileDescriptor &&other) noexcept : value_(other.release()) {}
    FileDescriptor &operator=(FileDescriptor &&other) noexcept {
        if (this != &other)
            reset(other.release());
        return *this;
    }
    ~FileDescriptor() {
        reset();
    }
    int get() const {
        return value_;
    }
    explicit operator bool() const {
        return value_ >= 0;
    }
    int release() noexcept {
        return std::exchange(value_, -1);
    }
    void reset(int value = -1) noexcept {
        const int previous = std::exchange(value_, value);
        // Linux releases the descriptor even when close reports EINTR.
        if (previous >= 0)
            ::close(previous);
    }
};
FileDescriptor above_stdio(int descriptor);
ssize_t write_pipe(int descriptor, const void *data, size_t size);
double monotonic_time();
double raw_time();
uint64_t raw_time_ns();
void sleep_for(double seconds);
} // namespace ct
