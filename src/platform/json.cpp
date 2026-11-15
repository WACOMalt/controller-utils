#include "json.h"
#include <cmath>
#include <limits>

namespace ct::json {
namespace {
void validate(json_object *object, unsigned depth) {
    if (depth > 64)
        throw std::invalid_argument("JSON nesting exceeded limit");
    if (!object)
        return;
    switch (json_object_get_type(object)) {
    case json_type_double:
        if (!std::isfinite(json_object_get_double(object)))
            throw std::invalid_argument("number must be finite");
        break;
    case json_type_array:
        for (size_t i = 0; i < json_object_array_length(object); ++i)
            validate(json_object_array_get_idx(object, i), depth + 1);
        break;
    case json_type_object: {
        json_object_object_foreach(object, key, value) {
            (void)key;
            validate(value, depth + 1);
        }
        break;
    }
    default:
        break;
    }
}
} // namespace
std::string encode(json_object *object, size_t limit) {
    validate(object, 0);
    size_t size = 0;
    const char *text = json_object_to_json_string_length(object, JSON_C_TO_STRING_PLAIN, &size);
    if (!text)
        throw std::bad_alloc();
    if (size > limit)
        throw std::invalid_argument("JSON message size exceeded");
    return {text, size};
}
Value parse(const std::string &text, size_t limit) {
    if (text.empty() || text.size() > limit ||
        text.size() > size_t(std::numeric_limits<int>::max()))
        throw std::invalid_argument("JSON message size exceeded");
    std::unique_ptr<json_tokener, decltype(&json_tokener_free)> parser(json_tokener_new(),
                                                                       json_tokener_free);
    if (!parser)
        throw std::bad_alloc();
    json_tokener_set_flags(parser.get(), JSON_TOKENER_STRICT | JSON_TOKENER_VALIDATE_UTF8);
    Value result(json_tokener_parse_ex(parser.get(), text.data(), int(text.size())));
    if (json_tokener_get_error(parser.get()) != json_tokener_success ||
        json_tokener_get_parse_end(parser.get()) != text.size() || !result)
        throw std::invalid_argument("invalid JSON message");
    return result;
}
json_object *find(json_object *object, const char *key) {
    if (!object || json_object_get_type(object) != json_type_object)
        throw std::invalid_argument("expected JSON object");
    json_object *result = nullptr;
    json_object_object_get_ex(object, key, &result);
    return result;
}
json_object *field(json_object *object, const char *key) {
    if (!object || json_object_get_type(object) != json_type_object)
        throw std::invalid_argument("expected JSON object");
    json_object *result = nullptr;
    if (!json_object_object_get_ex(object, key, &result))
        throw std::invalid_argument(std::string("missing field: ") + key);
    return result;
}
double number(json_object *object) {
    if (!object || (json_object_get_type(object) != json_type_int &&
                    json_object_get_type(object) != json_type_double))
        throw std::invalid_argument("expected JSON number");
    const double result = json_object_get_double(object);
    if (!std::isfinite(result))
        throw std::invalid_argument("number must be finite");
    return result;
}
int64_t integer(json_object *object) {
    if (!object || json_object_get_type(object) != json_type_int)
        throw std::invalid_argument("expected JSON integer");
    return json_object_get_int64(object);
}
uint64_t unsigned_integer(json_object *object) {
    if (!object || json_object_get_type(object) != json_type_int ||
        json_object_get_int64(object) < 0)
        throw std::invalid_argument("expected unsigned JSON integer");
    return json_object_get_uint64(object);
}
bool boolean(json_object *object) {
    if (!object || json_object_get_type(object) != json_type_boolean)
        throw std::invalid_argument("expected JSON boolean");
    return json_object_get_boolean(object);
}
std::string string(json_object *object) {
    if (!object || json_object_get_type(object) != json_type_string)
        throw std::invalid_argument("expected JSON string");
    return {json_object_get_string(object), size_t(json_object_get_string_len(object))};
}
size_t array_size(json_object *object) {
    if (!object || json_object_get_type(object) != json_type_array)
        throw std::invalid_argument("expected JSON array");
    return json_object_array_length(object);
}
json_object *item(json_object *object, size_t index) {
    if (index >= array_size(object))
        throw std::out_of_range("array index out of range");
    return json_object_array_get_idx(object, index);
}
std::optional<double> optional_number(json_object *object) {
    return object ? std::optional<double>(number(object)) : std::nullopt;
}
json_object *value(std::optional<double> number) {
    return number ? json_object_new_double(*number) : nullptr;
}
} // namespace ct::json
