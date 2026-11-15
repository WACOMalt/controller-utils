#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <json-c/json.h>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>

namespace ct::json {
struct Delete {
    void operator()(json_object *object) const {
        if (object)
            json_object_put(object);
    }
};
using Value = std::unique_ptr<json_object, Delete>;
Value parse(const std::string &text, size_t limit = 131072);
std::string encode(json_object *object, size_t limit = 131072);
json_object *field(json_object *object, const char *key);
json_object *find(json_object *object, const char *key);
double number(json_object *object);
int64_t integer(json_object *object);
uint64_t unsigned_integer(json_object *object);
bool boolean(json_object *object);
std::string string(json_object *object);
size_t array_size(json_object *object);
json_object *item(json_object *object, size_t index);
std::optional<double> optional_number(json_object *object);
json_object *value(std::optional<double> number);
template <size_t N> std::array<double, N> numbers(json_object *object) {
    if (array_size(object) != N)
        throw std::invalid_argument("unexpected vector size");
    std::array<double, N> result{};
    for (size_t i = 0; i < N; ++i)
        result[i] = number(item(object, i));
    return result;
}
template <size_t N> json_object *value(const std::array<double, N> &values) {
    auto result = json_object_new_array_ext(N);
    for (double v : values)
        json_object_array_add(result, json_object_new_double(v));
    return result;
}
} // namespace ct::json
