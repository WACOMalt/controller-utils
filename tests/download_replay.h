#pragma once
#include "update/download.h"
#include "platform/linux/files.h"
#include "platform/linux/error.h"
#include "platform/linux/resource.h"
#include "platform/json.h"
#include <atomic>
#include <filesystem>
#include <thread>

namespace download_replay {
inline size_t descriptors() {
    size_t result = 0;
    for (const auto &entry : std::filesystem::directory_iterator("/proc/self/fd")) {
        (void)entry;
        ++result;
    }
    return result;
}
inline ct::json::Value replay(json_object *request) {
    namespace j = ct::json;
    const auto before = descriptors();
    const auto started = ct::monotonic_time();
    j::Value result(json_object_new_object());
    {
        std::atomic_bool cancel{false};
        std::atomic<double> shift{0};
        const auto mode = j::string(j::field(request, "mode"));
        std::thread signal;
        if (mode == "already_cancelled")
            cancel = true;
        else if (mode == "cancel" || mode == "replace_cancel" || mode == "deadline" ||
                 mode == "cancel_dns")
            signal = std::thread([&] {
                ct::sleep_for(mode == "replace_cancel" ? 1 : .1);
                if (mode == "deadline")
                    shift = 121;
                else
                    cancel = true;
            });
        try {
            ct::update::HttpsDownloader client(j::string(j::field(request, "ca")),
                                               [&] { return ct::monotonic_time() + shift.load(); });
            const auto output = j::string(j::field(request, "destination"));
            const auto count =
                client.download(j::string(j::field(request, "url")), output,
                                j::unsigned_integer(j::field(request, "limit")), cancel);
            json_object_object_add(result.get(), "bytes", json_object_new_uint64(count));
            json_object_object_add(result.get(), "sha256",
                                   json_object_new_string(ct::sha256(output).c_str()));
        } catch (const std::exception &error) {
            json_object_object_add(result.get(), "error", json_object_new_string(error.what()));
            json_object_object_add(
                result.get(), "kind",
                json_object_new_string(dynamic_cast<const ct::update::DownloadCancelled *>(&error)
                                           ? "cancelled"
                                       : dynamic_cast<const ct::TimeoutError *>(&error) ? "timeout"
                                                                                        : "error"));
        }
        if (signal.joinable())
            signal.join();
    }
    json_object_object_add(result.get(), "elapsed",
                           json_object_new_double(ct::monotonic_time() - started));
    json_object_object_add(result.get(), "fds_before", json_object_new_uint64(before));
    json_object_object_add(result.get(), "fds_after", json_object_new_uint64(descriptors()));
    return result;
}
} // namespace download_replay
