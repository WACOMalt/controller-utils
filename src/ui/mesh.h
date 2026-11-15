#pragma once
#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <istream>
#include <stdexcept>
#include <vector>

namespace ct {
struct Vertex {
    float position[3], normal[3], uv[2]{}, part = 0, touchpad = 0;
};

struct Surface {
    uint32_t width = 0, height = 0;
    std::vector<unsigned char> color_roughness, normal_metalness;
};

inline uint64_t mesh_hash(const std::vector<Vertex> &vertices) {
    uint64_t hash = 14695981039346656037ULL;
    static_assert(offsetof(Vertex, normal) == 3 * sizeof(float));
    for (const auto &vertex : vertices) {
        const auto *bytes = reinterpret_cast<const unsigned char *>(vertex.position);
        for (size_t i = 0; i < 6 * sizeof(float); ++i)
            hash = (hash ^ bytes[i]) * 1099511628211ULL;
    }
    return hash;
}

inline Surface read_surface(std::istream &input, std::vector<Vertex> &vertices) {
    std::array<unsigned char, 28> header{};
    if (!input.read(reinterpret_cast<char *>(header.data()), header.size()) ||
        std::memcmp(header.data(), "CTAP0001", 8))
        throw std::runtime_error("invalid appearance header");
    const auto integer = [&](size_t offset, size_t size) {
        uint64_t value = 0;
        for (size_t i = 0; i < size; ++i)
            value |= uint64_t(header[offset + i]) << (8 * i);
        return value;
    };
    if (integer(8, 4) != vertices.size() || integer(20, 8) != mesh_hash(vertices))
        throw std::runtime_error("appearance does not match the controller geometry");
    Surface result;
    result.width = integer(12, 4);
    result.height = integer(16, 4);
    for (const auto size : {result.width, result.height})
        if (size < 64 || size > 2048 || (size & (size - 1)))
            throw std::runtime_error("appearance texture exceeds bounds");
    for (auto &vertex : vertices) {
        std::array<float, 5> values{};
        if (!input.read(reinterpret_cast<char *>(values.data()), sizeof(values)))
            throw std::runtime_error("truncated appearance coordinates");
        for (const auto value : values)
            if (!std::isfinite(value))
                throw std::runtime_error("nonfinite appearance coordinate");
        const float norm = values[0] * values[0] + values[1] * values[1] + values[2] * values[2];
        if (std::abs(norm - 1) > 1e-3f || values[3] < 0 || values[3] > 1 || values[4] < 0 ||
            values[4] > 1)
            throw std::runtime_error("invalid appearance normal or UV");
        std::copy_n(values.data(), 3, vertex.normal);
        std::copy_n(values.data() + 3, 2, vertex.uv);
    }
    for (auto *image : {&result.color_roughness, &result.normal_metalness}) {
        image->resize(size_t(result.width) * result.height * 4);
        if (!input.read(reinterpret_cast<char *>(image->data()), image->size()))
            throw std::runtime_error("truncated appearance texture");
    }
    if (input.peek() != std::char_traits<char>::eof())
        throw std::runtime_error("trailing appearance data");
    return result;
}

inline std::vector<Vertex> read_mesh(std::istream &input) {
    std::array<unsigned char, 84> header{};
    if (!input.read(reinterpret_cast<char *>(header.data()), header.size()))
        throw std::runtime_error("truncated STL header");
    const uint32_t count = uint32_t(header[80]) | uint32_t(header[81]) << 8 |
                           uint32_t(header[82]) << 16 | uint32_t(header[83]) << 24;
    if (!count || count > 100000)
        throw std::runtime_error("STL triangle count exceeds bounds");
    const uint32_t endian = 1;
    if (*reinterpret_cast<const unsigned char *>(&endian) != 1)
        throw std::runtime_error("STL requires a little-endian host");
    std::vector<Vertex> vertices;
    vertices.reserve(count * 3);
    for (uint32_t i = 0; i < count; ++i) {
        std::array<unsigned char, 50> bytes{};
        if (!input.read(reinterpret_cast<char *>(bytes.data()), bytes.size()))
            throw std::runtime_error("truncated STL triangle");
        std::array<float, 12> values{};
        std::memcpy(values.data(), bytes.data(), 48);
        for (float value : values)
            if (!std::isfinite(value))
                throw std::runtime_error("nonfinite STL value");
        const float norm = values[0] * values[0] + values[1] * values[1] + values[2] * values[2];
        if (std::abs(norm - 1) > 1e-3f)
            throw std::runtime_error("STL normals must be unit length");
        for (int v = 0; v < 3; ++v) {
            Vertex vertex{};
            for (int axis = 0; axis < 3; ++axis) {
                vertex.normal[axis] = values[axis];
                vertex.position[axis] = values[3 + v * 3 + axis];
                if (std::abs(vertex.position[axis]) > .3f)
                    throw std::runtime_error("controller mesh must be in metres");
            }
            vertices.push_back(vertex);
        }
    }
    if (input.peek() != std::char_traits<char>::eof())
        throw std::runtime_error("trailing STL data");
    return vertices;
}
} // namespace ct
