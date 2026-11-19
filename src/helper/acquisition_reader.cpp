#include "acquisition_reader.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <stdexcept>

namespace ct::helper {
namespace {
uintptr_t at(uintptr_t address, uintptr_t offset) {
    if (address > UINTPTR_MAX - offset)
        throw std::invalid_argument("native field address overflow");
    return address + offset;
}
template <class T> T load(const Bytes &bytes, size_t offset) {
    if (offset > bytes.size() || bytes.size() - offset < sizeof(T))
        throw std::invalid_argument("truncated native field");
    T value;
    std::memcpy(&value, bytes.data() + offset, sizeof(value));
    return value;
}
json_object *finite(double value) {
    return json::value(std::isfinite(value) ? std::optional<double>(value) : std::nullopt);
}
json_object *list(const Bytes &bytes) {
    auto result = json_object_new_array_ext(int(bytes.size()));
    for (const auto value : bytes)
        json_object_array_add(result, json_object_new_int(value));
    return result;
}
json_object *doubles(const Bytes &bytes, size_t offset, size_t count) {
    auto result = json::Value(json_object_new_array_ext(int(count)));
    for (size_t i = 0; i < count; ++i)
        json_object_array_add(result.get(), finite(load<double>(bytes, offset + 8 * i)));
    return result.release();
}
size_t vector_count(const Bytes &header, size_t offset, size_t stride, size_t maximum,
                    size_t capacity_limit, const char *error) {
    const auto begin = load<uint64_t>(header, offset), end = load<uint64_t>(header, offset + 8),
               capacity = load<uint64_t>(header, offset + 16);
    if (begin > end || end > capacity || (end - begin) % stride || end - begin > stride * maximum ||
        capacity - begin > stride * capacity_limit)
        throw std::invalid_argument(error);
    return size_t((end - begin) / stride);
}
} // namespace
AcquisitionReader::AcquisitionReader(MemoryRead read, std::function<double()> raw_time)
    : read_(std::move(read)), raw_time_(raw_time ? std::move(raw_time) : ct::raw_time) {
    if (!read_)
        throw std::invalid_argument("missing native acquisition reader");
}
Bytes AcquisitionReader::bytes(uintptr_t address, size_t size) {
    if (!size || size > 4096 || address > UINTPTR_MAX - size)
        throw std::invalid_argument("unreadable or out-of-bounds native field");
    auto result = read_(address, size);
    if (result.size() != size)
        throw std::invalid_argument("short native field read");
    return result;
}
uintptr_t AcquisitionReader::pointer(uintptr_t address) {
    const auto value = field<uint64_t>(address);
    if (value && value % 8)
        throw std::invalid_argument("unaligned native pointer");
    return value;
}
json::Value AcquisitionReader::record(uintptr_t address) {
    const auto header = bytes(address, 0xda0);
    const auto count =
        vector_count(header, 0x368, 48, 8, 64, "invalid or changing observation vector");
    json::Value result(json_object_new_object());
    auto observations = json_object_new_array_ext(int(count));
    json_object_object_add(result.get(), "observations", observations);
    if (count) {
        const auto begin = load<uint64_t>(header, 0x368);
        const auto data = bytes(begin, count * 48);
        for (size_t i = 0; i < count; ++i) {
            const auto offset = i * 48;
            const auto camera = load<int32_t>(data, offset), slot = load<int32_t>(data, offset + 4);
            if (camera < -1 || camera > 3 || slot < -1 || slot > 1)
                throw std::invalid_argument("unexpected observation identity");
            auto value = json_object_new_object();
            json_object_array_add(observations, value);
            json_object_object_add(value, "camera", json_object_new_int(camera));
            json_object_object_add(value, "slot", json_object_new_int(slot));
            json_object_object_add(value, "x", finite(load<double>(data, offset + 16)));
            json_object_object_add(value, "y", finite(load<double>(data, offset + 24)));
            json_object_object_add(value, "center_intensity",
                                   finite(load<float>(data, offset + 32)));
        }
        if (bytes(begin, count * 48) != data)
            throw std::invalid_argument("observation payload changed during read");
    }
    if (bytes(address, 0xda0) != header)
        throw std::invalid_argument("native record changed during read");
    json_object_object_add(result.get(), "valid", json_object_new_boolean(header[0] & 1));
    json_object_object_add(result.get(), "time", finite(load<double>(header, 0x70)));
    json_object_object_add(result.get(), "pose_source_raw", json_object_new_int(header[0x358]));
    json_object_object_add(result.get(), "palm_gate_raw",
                           json_object_new_uint64(load<uint32_t>(header, 0xd18)));
    json_object_object_add(result.get(), "palm_request_time", finite(load<double>(header, 0xd20)));
    json_object_object_add(result.get(), "palm_observation_time",
                           finite(load<double>(header, 0xd90)));
    return result;
}
json::Value AcquisitionReader::feature_group(uintptr_t address) {
    const auto header = bytes(address, 0xa8);
    json::Value result(json_object_new_object());
    const auto counts = json_object_new_array(), finite_counts = json_object_new_array(),
               rms = json_object_new_array(), ranges = json_object_new_array();
    json_object_object_add(result.get(), "counts", counts);
    json_object_object_add(result.get(), "finite_counts", finite_counts);
    json_object_object_add(result.get(), "rms", rms);
    json_object_object_add(result.get(), "ranges", ranges);
    const size_t strides[]{16, 16, 24, 16, 16, 24, 24};
    for (size_t i = 0; i < 7; ++i) {
        const auto stride = strides[i],
                   count = vector_count(header, i * 24, stride, 128, 1024,
                                        "invalid or changing native feature vector");
        const auto begin = load<uint64_t>(header, i * 24);
        const auto data = count ? bytes(begin, count * stride) : Bytes{};
        unsigned good = 0;
        double sum = 0;
        std::optional<double> minimum, maximum;
        for (size_t row = 0; row < count; ++row) {
            bool usable = true;
            double squares = 0;
            for (size_t offset = 0; offset < stride; offset += 8) {
                const auto value = load<double>(data, row * stride + offset);
                const bool valid = std::isfinite(value);
                usable = usable && valid;
                squares += value * value;
                if (valid) {
                    minimum = minimum ? std::min(*minimum, value) : value;
                    maximum = maximum ? std::max(*maximum, value) : value;
                }
            }
            sum += squares;
            good += usable;
        }
        json_object_array_add(counts, json_object_new_uint64(count));
        json_object_array_add(finite_counts, json_object_new_int(good));
        json_object_array_add(rms, count ? finite(std::sqrt(sum / double(count))) : nullptr);
        auto range = minimum ? json_object_new_array_ext(2) : nullptr;
        if (range) {
            json_object_array_add(range, json_object_new_double(*minimum));
            json_object_array_add(range, json_object_new_double(*maximum));
        }
        json_object_array_add(ranges, range);
        if (count && bytes(begin, count * stride) != data)
            throw std::invalid_argument("native feature payload changed");
    }
    if (bytes(address, 0xa8) != header)
        throw std::invalid_argument("native feature headers changed");
    return result;
}
json::Value AcquisitionReader::palm_estimator(uintptr_t address, bool include_pose) {
    const auto header = bytes(address, 0x2a8);
    std::array<size_t, 4> counts{};
    const std::pair<size_t, size_t> vectors[]{
        {0, 0x3a0}, {0xa0, 0x80}, {0xd0, 0xc0}, {0x130, 0x50}};
    for (size_t i = 0; i < 4; ++i)
        counts[i] = vector_count(header, vectors[i].first, vectors[i].second, 4096, 8192,
                                 "invalid or changing palm estimator vector");
    json::Value result(json_object_new_object());
    json_object_object_add(result.get(), "history_count", json_object_new_uint64(counts[0]));
    json_object_object_add(result.get(), "accepted", json_object_new_boolean(header[0x22] & 1));
    auto residuals = json_object_new_array_ext(3);
    json_object_object_add(result.get(), "residual_counts_raw", residuals);
    for (size_t i = 1; i < 4; ++i)
        json_object_array_add(residuals, json_object_new_uint64(counts[i]));
    json_object_object_add(result.get(), "residual_flags_raw",
                           list(Bytes(header.begin() + 0x175, header.begin() + 0x177)));
    auto costs = json_object_new_array_ext(2);
    json_object_object_add(result.get(), "costs_raw", costs);
    for (const size_t offset : {0x270, 0x2a0})
        json_object_array_add(costs, finite(load<double>(header, offset)));
    if (counts[0]) {
        const auto end = load<uint64_t>(header, 8);
        const auto latest = bytes(end - 0x3a0, 0x90);
        json_object_object_add(result.get(), "latest_time", finite(load<double>(latest, 0)));
        bool valid = true;
        for (size_t i = 0; i < 7; ++i)
            valid = valid && std::isfinite(load<double>(latest, 0x20 + i * 8));
        if (valid && include_pose)
            json_object_object_add(result.get(), "pose", doubles(latest, 0x20, 7));
        json_object_object_add(result.get(), "latest_pose_finite", json_object_new_boolean(valid));
        json_object_object_add(result.get(), "covariance_diagonal_raw", doubles(latest, 0x60, 6));
        if (bytes(end - 0x3a0, 0x90) != latest)
            throw std::invalid_argument("palm estimator latest record changed");
    }
    if (bytes(address, 0x2a8) != header)
        throw std::invalid_argument("palm estimator changed during read");
    return result;
}
json::Value AcquisitionReader::snapshot(std::optional<uintptr_t> wrapper_override,
                                        bool hands_only) {
    const auto app = pointer(AppGlobal);
    if (!app || pointer(app) != AppVtable)
        throw std::invalid_argument("XRService app identity mismatch");
    const auto stock_wrapper = pointer(at(app, 0x1970));
    const auto wrapper = wrapper_override.value_or(stock_wrapper);
    if (!wrapper || wrapper % 8)
        throw std::invalid_argument("invalid native wrapper");
    const auto manager = pointer(wrapper);
    if (wrapper_override && (!manager || pointer(at(manager, 0x3b8)) != app))
        throw std::invalid_argument("private native manager app identity mismatch");
    const auto classic = manager ? pointer(at(manager, 0x2b0)) : 0;
    json::Value result(json_object_new_object());
    json_object_object_add(result.get(), "raw_time", finite(raw_time_()));
    json_object_object_add(result.get(), "classic_present", json_object_new_boolean(bool(classic)));
    const auto read_palms = [&] {
        auto palms = json_object_new_array_ext(2);
        json_object_object_add(result.get(), "palm_estimators_raw", palms);
        for (unsigned side = 0; side < 2; ++side) {
            auto palm = palm_estimator(at(classic, 0x3c0 + side * 0x350), hands_only);
            if (hands_only) {
                json::Value row(json_object_new_object());
                for (const auto key : {"accepted", "latest_time", "pose"})
                    if (auto field = json::find(palm.get(), key))
                        json_object_object_add(row.get(), key, json_object_get(field));
                palm = std::move(row);
            } else {
                json_object_object_add(palm.get(), "side_index", json_object_new_int(side));
            }
            json_object_array_add(palms, palm.release());
        }
    };
    const auto verify_tracker = [&] {
        if (pointer(AppGlobal) != app || pointer(at(app, 0x1970)) != stock_wrapper ||
            pointer(wrapper) != manager || pointer(at(manager, 0x2b0)) != classic)
            throw std::invalid_argument("native tracker changed during read");
    };
    if (hands_only) {
        if (classic && manager)
            read_palms();
        if (manager)
            verify_tracker();
        return result;
    }
    if (manager) {
        auto gates = json_object_new_object();
        json_object_object_add(result.get(), "manager_request_gates_raw", gates);
        json_object_object_add(gates, "flag_3d8",
                               json_object_new_int(field<uint8_t>(at(manager, 0x3d8))));
        json_object_object_add(gates, "flag_2",
                               json_object_new_int(field<uint8_t>(at(manager, 2))));
        json_object_object_add(gates, "vector_4c8", doubles(bytes(at(manager, 0x4c8), 24), 0, 3));
    }
    if (!classic) {
        if (pointer(AppGlobal) != app || pointer(at(app, 0x1970)) != stock_wrapper)
            throw std::invalid_argument("native tracker changed during read");
        return result;
    }
    json_object_object_add(result.get(), "frame_kind",
                           json_object_new_uint64(field<uint32_t>(at(classic, 0x12a0))));
    json_object_object_add(result.get(), "preset_mode",
                           json_object_new_uint64(field<uint32_t>(at(classic, 0x9588))));
    json_object_object_add(result.get(), "palm_flags", list(bytes(at(classic, 0x1194), 6)));
    json_object_object_add(result.get(), "palm_bootstrap_min_frames",
                           json_object_new_int(field<int32_t>(at(classic, 0xd90))));
    json_object_object_add(result.get(), "palm_frame_gate_raw",
                           json_object_new_uint64(field<uint32_t>(at(classic, 0xb58))));
    json_object_object_add(result.get(), "palm_filter_gate_enabled",
                           json_object_new_boolean(field<uint8_t>(at(classic, 0x95c1)) & 1));
    auto slots = json_object_new_array_ext(2);
    json_object_object_add(result.get(), "slots", slots);
    bool connected_any = false;
    for (unsigned index = 0; index < 2; ++index) {
        const auto slot = at(classic, 0x15c0 + index * 0x2660);
        const bool connected = field<uint8_t>(at(slot, 0x2508)) == 1;
        connected_any = connected_any || connected;
        auto item = json_object_new_object();
        json_object_array_add(slots, item);
        json_object_object_add(item, "index", json_object_new_int(index));
        json_object_object_add(item, "connected", json_object_new_boolean(connected));
        if (!connected)
            continue;
        const auto model = pointer(at(slot, 8));
        if (model) {
            const auto vector = bytes(at(model, 8), 16);
            const auto begin = load<uint64_t>(vector, 0), end = load<uint64_t>(vector, 8);
            if (end < begin || (end - begin) % 0x98 || end - begin > 128 * 0x98)
                throw std::invalid_argument("unexpected native LED model");
            auto info = json_object_new_object();
            json_object_object_add(item, "model", info);
            json_object_object_add(info, "type",
                                   json_object_new_uint64(field<uint32_t>(at(model, 0x190))));
            json_object_object_add(info, "role",
                                   json_object_new_uint64(field<uint32_t>(at(model, 0x200))));
            json_object_object_add(info, "led_count", json_object_new_uint64((end - begin) / 0x98));
            const auto thresholds = bytes(at(model, 0x1e0), 8);
            auto values = json_object_new_array_ext(2);
            json_object_object_add(info, "thresholds", values);
            for (size_t i = 0; i < 2; ++i)
                json_object_array_add(values,
                                      json_object_new_int(load<int32_t>(thresholds, i * 4)));
        }
        const auto filter = pointer(at(slot, 0x40));
        json_object_object_add(item, "filter_time",
                               filter ? finite(field<double>(at(filter, 0x21f0))) : nullptr);
        json_object_object_add(item, "palm_slot_gate_raw",
                               json_object_new_uint64(field<uint32_t>(at(slot, 0x1b58))));
        json_object_object_add(item, "palm_filter_state_raw",
                               filter ? json_object_new_int(field<int32_t>(at(filter, 0x20e0)))
                                      : nullptr);
        json_object_object_add(item, "filter_gate_2400_raw",
                               filter ? json_object_new_int(field<uint8_t>(at(filter, 0x2400)))
                                      : nullptr);
        json_object_object_add(item, "previous_record_valid",
                               json_object_new_boolean(field<uint8_t>(at(slot, 0x90)) & 1));
        json_object_object_add(item, "previous_record_counter_raw",
                               json_object_new_int(field<int32_t>(at(slot, 0x94))));
        json_object_object_add(item, "palm_covariance_limits",
                               filter ? doubles(bytes(at(filter, 0x1918), 16), 0, 2) : nullptr);
        const auto seed = bytes(at(classic, 0x13c0 + index * 0xe0), 16);
        auto palm = json_object_new_object();
        json_object_object_add(item, "palm_seed", palm);
        json_object_object_add(palm, "status", json_object_new_int(load<int32_t>(seed, 0)));
        json_object_object_add(palm, "side", json_object_new_int(load<int32_t>(seed, 4)));
        json_object_object_add(palm, "time", finite(load<double>(seed, 8)));
        json_object_object_add(item, "current", record(at(slot, 0xe40)).release());
        const auto ring = bytes(at(slot, 0x60), 24);
        const auto capacity = load<int32_t>(ring, 0), start = load<int32_t>(ring, 4),
                   count = load<int32_t>(ring, 8);
        const auto base = load<uint64_t>(ring, 16);
        if (count < 0 || count > capacity || capacity > 10000 ||
            !((capacity == 0 && start == 0) || (start >= 0 && start < capacity)))
            throw std::invalid_argument("invalid or changing native history ring");
        json_object_object_add(item, "history_count", json_object_new_int(count));
        if (count)
            json_object_object_add(
                item, "latest",
                record(at(base, uintptr_t((start + count - 1) % capacity) * 0xda0)).release());
        if (bytes(at(slot, 0x60), 24) != ring)
            throw std::invalid_argument("history changed during read");
        if ((field<uint8_t>(at(slot, 0x2508)) == 1) != connected || pointer(at(slot, 8)) != model ||
            pointer(at(slot, 0x40)) != filter)
            throw std::invalid_argument("controller changed during read");
    }
    if (connected_any) {
        auto groups = json_object_new_array_ext(4);
        json_object_object_add(result.get(), "feature_groups_raw", groups);
        for (unsigned index = 0; index < 2; ++index) {
            for (const auto &[name, offset] :
                 std::array<std::pair<const char *, uintptr_t>, 2>{{{"A", 0x480}, {"B", 0x1e0}}}) {
                auto group = json_object_new_object();
                json_object_array_add(groups, group);
                json_object_object_add(group, "index", json_object_new_int(index));
                json_object_object_add(group, "group", json_object_new_string(name));
                auto cameras = json_object_new_array_ext(2);
                json_object_object_add(group, "cameras", cameras);
                for (unsigned camera = 0; camera < 2; ++camera)
                    json_object_array_add(
                        cameras,
                        feature_group(at(classic, 0x6280 + index * 0x150 + offset + camera * 0xa8))
                            .release());
            }
        }
        read_palms();
    }
    verify_tracker();
    return result;
}
} // namespace ct::helper

namespace ct::helper {
AcquisitionQuality acquisition_quality(json_object *state) {
    AcquisitionQuality result;
    result.time = json::optional_number(json::find(state, "raw_time"));
    if (auto slots = json::find(state, "slots")) {
        for (size_t i = 0; i < json::array_size(slots); ++i) {
            auto slot = json::item(slots, i);
            AcquisitionQuality::Slot entry;
            entry.index = int(json::integer(json::field(slot, "index")));
            if (auto connected = json::find(slot, "connected"))
                entry.connected = json::boolean(connected);
            if (auto model = json::find(slot, "model"))
                if (auto count = json::find(model, "led_count"))
                    entry.led_count = unsigned(json::integer(count));
            if (auto seed = json::find(slot, "palm_seed")) {
                if (auto status = json::find(seed, "status"))
                    entry.seed_status = int(json::integer(status));
                if (auto side = json::find(seed, "side"))
                    entry.seed_side = int(json::integer(side));
                entry.seed_time = json::optional_number(json::find(seed, "time"));
            }
            result.slots.push_back(entry);
        }
    }
    if (auto palms = json::find(state, "palm_estimators_raw")) {
        for (size_t i = 0; i < json::array_size(palms); ++i) {
            auto palm = json::item(palms, i);
            AcquisitionQuality::Palm entry;
            entry.side = int(json::integer(json::field(palm, "side_index")));
            if (auto accepted = json::find(palm, "accepted"))
                entry.accepted = json::boolean(accepted);
            if (auto diagonal = json::find(palm, "covariance_diagonal_raw"))
                for (size_t n = 0; n < json::array_size(diagonal); ++n) {
                    auto value = json::item(diagonal, n);
                    entry.covariance.push_back(value ? json::number(value)
                                                     : std::numeric_limits<double>::quiet_NaN());
                }
            result.palms.push_back(std::move(entry));
        }
    }
    if (auto groups = json::find(state, "feature_groups_raw")) {
        for (size_t i = 0; i < json::array_size(groups); ++i) {
            auto group = json::item(groups, i);
            if (json::string(json::field(group, "group")) != "B")
                continue;
            AcquisitionQuality::Stereo entry;
            entry.slot = int(json::integer(json::field(group, "index")));
            auto cameras = json::field(group, "cameras");
            for (size_t n = 0; n < json::array_size(cameras); ++n) {
                auto camera = json::item(cameras, n);
                entry.cameras.push_back(
                    {unsigned(json::integer(json::item(json::field(camera, "counts"), 3))),
                     unsigned(json::integer(json::item(json::field(camera, "finite_counts"), 3))),
                     json::optional_number(json::item(json::field(camera, "rms"), 3))});
            }
            result.groups.push_back(std::move(entry));
        }
    }
    return result;
}
} // namespace ct::helper
