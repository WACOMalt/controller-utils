#include "ui/mesh.h"
#include <cassert>
#include <fstream>
#include <iostream>
#include <limits>
#include <sstream>

int main() {
    std::ifstream file("assets/controller.stl", std::ios::binary);
    const auto vertices = ct::read_mesh(file);
    assert(vertices.size() == 150000);
    float min_x = 1, max_x = -1;
    for (const auto &v : vertices) {
        min_x = std::min(min_x, v.position[0]);
        max_x = std::max(max_x, v.position[0]);
    }
    assert(max_x - min_x > .15f && max_x - min_x < .17f);
    for (int mode = 0; mode < 4; ++mode) {
        std::string data(134, '\0');
        data[80] = 1;
        float values[12] = {0, 0, 1, 0, 0, 0, .1f, 0, 0, 0, .1f, 0};
        if (mode == 0)
            data.resize(100);
        else {
            if (mode == 1)
                values[8] = 100;
            if (mode == 2)
                values[2] = 0;
            std::memcpy(data.data() + 84, values, sizeof(values));
            if (mode == 3)
                data += 'x';
        }
        std::istringstream input(data);
        bool rejected = false;
        try {
            (void)ct::read_mesh(input);
        } catch (const std::runtime_error &) {
            rejected = true;
        }
        assert(rejected);
    }
    {
        auto styled = vertices;
        std::ifstream appearance("assets/controller.surface", std::ios::binary);
        const auto surface = ct::read_surface(appearance, styled);
        assert(surface.width == 2048 && surface.height == 2048);
        for (size_t i = 0; i < vertices.size(); ++i)
            for (size_t axis = 0; axis < 3; ++axis)
                assert(styled[i].position[axis] == vertices[i].position[axis]);
        auto mismatched = vertices;
        mismatched[0].position[0] += .0001f;
        appearance.clear();
        appearance.seekg(0);
        bool rejected = false;
        try {
            (void)ct::read_surface(appearance, mismatched);
        } catch (const std::runtime_error &) {
            rejected = true;
        }
        assert(rejected);
    }
    std::vector<ct::Vertex> triangle(3);
    for (auto &vertex : triangle)
        vertex.normal[2] = 1;
    std::string valid(28 + 3 * 20 + 64 * 64 * 8, '\0');
    valid.replace(0, 8, "CTAP0001");
    const auto integer = [&](size_t offset, uint64_t value, size_t count) {
        for (size_t i = 0; i < count; ++i)
            valid[offset + i] = char(value >> (8 * i));
    };
    integer(8, 3, 4);
    integer(12, 64, 4);
    integer(16, 64, 4);
    integer(20, ct::mesh_hash(triangle), 8);
    for (int i = 0; i < 3; ++i) {
        const float values[5] = {0, 0, 1, .5f, .5f};
        std::memcpy(valid.data() + 28 + 20 * i, values, sizeof(values));
    }
    for (int mode = 0; mode < 8; ++mode) {
        auto data = valid;
        if (mode == 0)
            data[0] = 'X';
        if (mode == 1)
            data[8] = 4;
        if (mode == 2)
            data[12] = 63;
        if (mode == 3)
            data.resize(40);
        if (mode == 4) {
            const float nan = std::numeric_limits<float>::quiet_NaN();
            std::memcpy(data.data() + 28, &nan, sizeof(nan));
        }
        if (mode == 5) {
            const float bad_uv = 1.1f;
            std::memcpy(data.data() + 40, &bad_uv, sizeof(bad_uv));
        }
        if (mode == 6)
            data.pop_back();
        if (mode == 7)
            data += 'x';
        auto input_vertices = triangle;
        std::istringstream input(data);
        bool rejected = false;
        try {
            (void)ct::read_surface(input, input_vertices);
        } catch (const std::runtime_error &) {
            rejected = true;
        }
        assert(rejected);
    }
    std::cout << "controller mesh: CAD dimensions, unchanged styled geometry, appearance matching "
                 "and malformed input checks passed\n";
}
