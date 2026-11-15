#pragma once
#include "interaction/pose_math.h"
#include <json-c/json.h>
#include <memory>
#include <string>

namespace ct {
struct BodyPose {
    double optical_time = 0, position_time = 0, orientation_time = 0;
    Matrix model = identity();
    bool raw = false;
    BodyPose in_standing(const Matrix &raw_to_standing) const {
        BodyPose result = *this;
        if (raw)
            result.model = multiply(raw_to_standing, model);
        result.raw = false;
        return result;
    }
    bool fresh(double now, double max_age = .05) const {
        return optical_time > 0 && now - optical_time >= -.02 && now - position_time >= -.02 &&
               now - position_time <= max_age && now - orientation_time >= -.02 &&
               now - orientation_time <= max_age;
    }
};

inline double number(json_object *object) {
    if (!object || (json_object_get_type(object) != json_type_double &&
                    json_object_get_type(object) != json_type_int))
        throw std::runtime_error("pose values must be numbers");
    const double value = json_object_get_double(object);
    if (!std::isfinite(value))
        throw std::runtime_error("pose values must be finite");
    return value;
}

inline json_object *field(json_object *object, const char *key) {
    json_object *value = nullptr;
    if (!json_object_object_get_ex(object, key, &value))
        throw std::runtime_error(std::string("missing pose field: ") + key);
    return value;
}

inline std::string parse_command(json_object *root, BodyPose &pose) {
    if (!root || json_object_get_type(root) != json_type_object)
        throw std::runtime_error("invalid renderer JSON");
    auto operation = field(root, "op");
    if (json_object_get_type(operation) != json_type_string)
        throw std::runtime_error("invalid renderer operation");
    const std::string op = json_object_get_string(operation);
    if ((op == "hide" || op == "quit") && json_object_object_length(root) == 1)
        return op;
    if ((op != "pose" && op != "native_pose") || json_object_object_length(root) != 6)
        throw std::runtime_error("unsupported renderer command");
    BodyPose next;
    next.raw = op == "native_pose";
    next.optical_time = number(field(root, "time"));
    next.position_time = number(field(root, "position_time"));
    next.orientation_time = number(field(root, "orientation_time"));
    if (next.optical_time <= 0 || next.orientation_time <= 0 || next.position_time <= 0 ||
        next.optical_time > next.orientation_time + .02 ||
        next.optical_time > next.position_time + .02 || next.optical_time < pose.optical_time ||
        next.orientation_time <= pose.orientation_time || next.position_time <= pose.position_time)
        throw std::runtime_error("unordered renderer timestamps");
    auto position = field(root, "position"), orientation = field(root, "orientation");
    if (json_object_get_type(position) != json_type_array ||
        json_object_array_length(position) != 3 ||
        json_object_get_type(orientation) != json_type_array ||
        json_object_array_length(orientation) != 4)
        throw std::runtime_error("invalid body pose dimensions");
    for (int i = 0; i < 3; ++i) {
        const double value = number(json_object_array_get_idx(position, i));
        if (std::abs(value) > 20)
            throw std::runtime_error("body position outside supported room");
        next.model[12 + i] = static_cast<float>(value);
    }
    double q[4], norm = 0;
    for (int i = 0; i < 4; ++i) {
        q[i] = number(json_object_array_get_idx(orientation, i));
        norm += q[i] * q[i];
    }
    if (std::abs(norm - 1) > .001)
        throw std::runtime_error("body quaternion must have unit length");
    const double w = q[0], x = q[1], y = q[2], z = q[3];
    next.model[0] = 1 - 2 * (y * y + z * z);
    next.model[4] = 2 * (x * y - z * w);
    next.model[8] = 2 * (x * z + y * w);
    next.model[1] = 2 * (x * y + z * w);
    next.model[5] = 1 - 2 * (x * x + z * z);
    next.model[9] = 2 * (y * z - x * w);
    next.model[2] = 2 * (x * z - y * w);
    next.model[6] = 2 * (y * z + x * w);
    next.model[10] = 1 - 2 * (x * x + y * y);
    (void)inverse_rigid(next.model);
    pose = next;
    return op;
}
inline std::string parse_command(const std::string &line, BodyPose &pose) {
    if (line.empty() || line.size() > 4096)
        throw std::runtime_error("renderer command size exceeded");
    std::unique_ptr<json_tokener, decltype(&json_tokener_free)> parser(json_tokener_new(),
                                                                       json_tokener_free);
    if (!parser)
        throw std::runtime_error("cannot allocate JSON parser");
    json_tokener_set_flags(parser.get(), JSON_TOKENER_STRICT);
    std::unique_ptr<json_object, decltype(&json_object_put)> root(
        json_tokener_parse_ex(parser.get(), line.data(), static_cast<int>(line.size())),
        json_object_put);
    if (json_tokener_get_error(parser.get()) != json_tokener_success ||
        json_tokener_get_parse_end(parser.get()) != line.size())
        throw std::runtime_error("invalid renderer JSON");
    return parse_command(root.get(), pose);
}
} // namespace ct
