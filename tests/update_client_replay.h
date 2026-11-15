#pragma once
#include "download_replay.h"
#include "update/client.h"
#include "update/transaction.h"
#include "platform/linux/files.h"
#include <atomic>
#include <algorithm>

namespace update_client_replay {
namespace j = ct::json;
inline size_t descriptors() {
    size_t count = 0;
    for (const auto &entry : std::filesystem::directory_iterator("/proc/self/fd")) {
        (void)entry;
        ++count;
    }
    return count;
}
inline j::Value replay(json_object *request) {
    const auto before = descriptors();
    j::Value result(json_object_new_object()), snapshots(json_object_new_array());
    const std::filesystem::path root = j::string(j::field(request, "root")),
                                data = j::string(j::field(request, "data"));
    double maximum_poll = 0, close_time = 0;
    unsigned captures = 0;
    {
        ct::ApplicationLock application(data / "app.lock");
        std::atomic<double> now{j::number(j::field(request, "now"))};
        const auto capture = [&] {
            ++captures;
            return ct::update::InstalledRequirements{
                j::string(j::field(request, "machine")),
                j::parse(j::encode(j::field(request, "requirements")))};
        };
        std::unique_ptr<ct::update::MaintenanceLock> blocked;
        if (j::boolean(j::field(request, "lease_busy")))
            blocked = std::make_unique<ct::update::MaintenanceLock>(data);
        ct::update::UpdateClient client(root, data, capture, j::string(j::field(request, "ca")),
                                        [&] { return now.load(); });
        const auto poll = [&](bool automatic) {
            const auto started = ct::monotonic_time();
            auto status = client.poll(automatic);
            maximum_poll = std::max(maximum_poll, ct::monotonic_time() - started);
            return status;
        };
        const auto steps = j::field(request, "steps");
        for (size_t index = 0; index < j::array_size(steps); ++index) {
            const auto step = j::item(steps, index);
            const auto operation = j::string(j::field(step, "op"));
            bool automatic = false;
            if (operation == "check")
                client.check(j::boolean(j::field(step, "manual")));
            else if (operation == "poll")
                automatic = j::boolean(j::field(step, "automatic"));
            else if (operation == "time")
                now = j::number(j::field(step, "now"));
            else if (operation == "sleep")
                ct::sleep_for(j::number(j::field(step, "seconds")));
            else if (operation == "wait") {
                const auto deadline = ct::monotonic_time() + 10;
                while (poll(false).busy) {
                    if (ct::monotonic_time() > deadline)
                        throw std::runtime_error("Native update test timed out");
                    ct::sleep_for(.005);
                }
            } else if (operation == "close") {
                const auto started = ct::monotonic_time();
                client.close();
                close_time = ct::monotonic_time() - started;
            } else if (operation == "activate") {
                client.close();
                ct::update::MaintenanceLock maintenance(data);
                ct::update::Transaction transaction(root, data, application, maintenance, capture);
                json_object_object_add(result.get(), "activated",
                                       json_object_new_boolean(transaction.activate()));
                ct::update::confirm(root, data, application);
            } else
                throw std::invalid_argument("Unknown native update test step");
            json_object_array_add(snapshots.get(), poll(automatic).to_json().release());
        }
        client.close();
    }
    json_object_object_add(result.get(), "snapshots", snapshots.release());
    json_object_object_add(result.get(), "captures", json_object_new_int64(captures));
    json_object_object_add(result.get(), "maximum_poll", json_object_new_double(maximum_poll));
    json_object_object_add(result.get(), "close_time", json_object_new_double(close_time));
    json_object_object_add(result.get(), "fds_before", json_object_new_uint64(before));
    const auto after = descriptors();
    json_object_object_add(result.get(), "fds_after", json_object_new_uint64(after));
    if (after != before)
        json_object_object_add(result.get(), "descriptor_targets",
                               download_replay::descriptor_targets().release());
    json_object_object_add(result.get(), "release", ct::update::release(root).to_json().release());
    return result;
}
} // namespace update_client_replay
