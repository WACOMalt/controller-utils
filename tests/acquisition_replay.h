#pragma once
#include "helper/acquisition_reader.h"
#include <map>
#include <tuple>

namespace acquisition_replay {
namespace j = ct::json;
inline ct::helper::Bytes decode(json_object *value) {
    const auto text = j::string(value);
    if (text.size() % 2)
        throw std::invalid_argument("invalid test memory encoding");
    ct::helper::Bytes result;
    for (size_t i = 0; i < text.size(); i += 2)
        result.push_back(uint8_t(std::stoul(text.substr(i, 2), nullptr, 16)));
    return result;
}
inline j::Value replay(json_object *request) {
    j::Value result(json_object_new_array());
    const auto steps = j::field(request, "steps");
    for (size_t index = 0; index < j::array_size(steps); ++index) {
        const auto step = j::item(steps, index);
        std::map<uintptr_t, uint8_t> memory;
        const auto regions = j::field(step, "memory");
        for (size_t i = 0; i < j::array_size(regions); ++i) {
            const auto segment = j::item(regions, i);
            const auto address = uintptr_t(j::unsigned_integer(j::item(segment, 0)));
            const auto bytes = decode(j::item(segment, 1));
            for (size_t n = 0; n < bytes.size(); ++n)
                memory[address + n] = bytes[n];
        }
        std::map<std::pair<uintptr_t, size_t>, size_t> calls;
        ct::helper::AcquisitionReader reader(
            [&](uintptr_t address, size_t size) {
                const auto occurrence = ++calls[{address, size}];
                if (const auto changes = j::find(step, "changes")) {
                    for (size_t i = 0; i < j::array_size(changes); ++i) {
                        const auto change = j::item(changes, i);
                        if (j::unsigned_integer(j::field(change, "address")) != address ||
                            j::unsigned_integer(j::field(change, "size")) != size ||
                            j::unsigned_integer(j::field(change, "occurrence")) != occurrence)
                            continue;
                        const auto write =
                            uintptr_t(j::unsigned_integer(j::field(change, "write")));
                        const auto bytes = decode(j::field(change, "bytes"));
                        for (size_t n = 0; n < bytes.size(); ++n)
                            memory[write + n] = bytes[n];
                    }
                }
                ct::helper::Bytes bytes(size);
                for (size_t i = 0; i < size; ++i)
                    bytes[i] = memory[address + i];
                return bytes;
            },
            [&] { return j::number(j::field(step, "now")); });
        j::Value value;
        try {
            const auto operation = j::string(j::field(step, "op"));
            if (operation == "snapshot") {
                std::optional<uintptr_t> wrapper;
                if (const auto field = j::find(step, "wrapper"))
                    wrapper = uintptr_t(j::unsigned_integer(field));
                auto snapshot = reader.snapshot(wrapper);
                const auto good = ct::helper::acquisition_quality(snapshot.get()).ready();
                value.reset(json_object_new_object());
                json_object_object_add(value.get(), "snapshot", snapshot.release());
                json_object_object_add(value.get(), "good", json_object_new_boolean(good));
            } else {
                const auto address = uintptr_t(j::unsigned_integer(j::field(step, "address")));
                if (operation == "record")
                    value = reader.record(address);
                else if (operation == "feature_group")
                    value = reader.feature_group(address);
                else if (operation == "palm_estimator")
                    value = reader.palm_estimator(address);
                else
                    throw std::invalid_argument("unknown acquisition test operation");
            }
        } catch (const std::exception &error) {
            value.reset(json_object_new_object());
            json_object_object_add(value.get(), "error", json_object_new_string(error.what()));
        }
        json_object_array_add(result.get(), value.release());
    }
    return result;
}
} // namespace acquisition_replay
