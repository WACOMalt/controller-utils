#include "ui/model_renderer.h"
#include <cairo/cairo.h>
#include <cassert>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <sstream>

namespace {
using Pixels = std::vector<unsigned char>;
using ct::ui::ModelRenderer;
using ct::ui::TouchpadGlow;
constexpr int Size = ModelRenderer::Size;

void state_checks() {
    TouchpadGlow glow;
    float left[3]{-.5f, .5f, 0}, right[3]{.5f, -.5f, 0};
    double now = 10;
    auto update = [&](uint32_t buttons, double elapsed = .01, bool visible = true) {
        now += elapsed;
        glow.update(buttons, left, right, now, now, visible);
    };
    update(0);
    assert(glow.contacts[0][2] == 0 && glow.contacts[1][2] == 0);
    update(TouchpadGlow::masks[0]);
    assert(glow.contacts[0][0] == -.5f && glow.contacts[0][1] == .5f);
    assert(glow.contacts[0][2] > 0 && glow.contacts[0][2] < 1 && glow.contacts[1][2] == 0);
    update(TouchpadGlow::masks[0], .03);
    assert(glow.contacts[0][2] == 1);
    update(TouchpadGlow::masks[0] | TouchpadGlow::masks[1], .04);
    assert(glow.contacts[0][2] == 1 && glow.contacts[1][2] == 1);
    assert(glow.contacts[1][0] == .5f && glow.contacts[1][1] == -.5f);
    for (int i = 0; i < 10; ++i) {
        left[0] = -.5f + (i % 2 ? .002f : -.002f);
        update(TouchpadGlow::masks[0] | TouchpadGlow::masks[1]);
        assert(glow.contacts[0][0] == -.5f);
    }
    left[0] = .5f;
    update(TouchpadGlow::masks[0] | TouchpadGlow::masks[1]);
    assert(glow.contacts[0][0] > -.5f && glow.contacts[0][0] < .5f);
    assert(glow.contacts[1][0] == .5f);
    const auto last = glow.contacts[0];
    update(TouchpadGlow::masks[1], .04);
    assert(glow.contacts[0][2] > 0 && glow.contacts[0][2] < 1);
    assert(glow.contacts[0][0] == last[0] && glow.contacts[0][1] == last[1]);
    update(TouchpadGlow::masks[1], .04);
    assert(glow.contacts[0][2] < 1e-5f && glow.contacts[1][2] == 1);
    left[0] = -.8f;
    update(TouchpadGlow::masks[0] | TouchpadGlow::masks[1]);
    assert(glow.contacts[0][0] == -.8f);
    update(0, .01, false);
    assert(glow.contacts == decltype(glow.contacts){});
    update(TouchpadGlow::masks[0]);
    glow.update(TouchpadGlow::masks[0], left, right, now, now + .151, true);
    assert(glow.contacts == decltype(glow.contacts){});
    update(TouchpadGlow::masks[0]);
    glow.update(TouchpadGlow::masks[0], left, right, now + .03, now, true);
    assert(glow.contacts == decltype(glow.contacts){});
    glow.update(TouchpadGlow::masks[0], left, right, now, std::numeric_limits<double>::quiet_NaN(),
                true);
    update(TouchpadGlow::masks[0]);
    assert(std::isfinite(glow.contacts[0][2]) && glow.contacts[0][2] > 0);
}

ct::Matrix projection() {
    auto result = ct::identity();
    result[0] = 2 / .19f;
    result[5] = 2 / .14f;
    result[10] = -2 / .9f;
    result[14] = -1.1f / .9f;
    return result;
}
ct::Matrix model() {
    auto result = ct::identity();
    result[5] = result[10] = 0;
    result[6] = 1;
    result[9] = -1;
    result[13] = .05f;
    result[14] = -.5f;
    return result;
}
Pixels pixels() {
    Pixels result(Size * Size * 4);
    glReadPixels(0, 0, Size, Size, GL_RGBA, GL_UNSIGNED_BYTE, result.data());
    assert(glGetError() == GL_NO_ERROR);
    return result;
}
Pixels render(ModelRenderer &gpu) {
    gpu.render(projection(), ct::identity(), model());
    return pixels();
}
void save(const Pixels &image, const std::filesystem::path &path) {
    std::vector<uint32_t> argb(Size * Size);
    for (int y = 0; y < Size; ++y)
        for (int x = 0; x < Size; ++x) {
            const size_t source = ((Size - 1 - y) * Size + x) * 4;
            argb[y * Size + x] = uint32_t(image[source + 3]) << 24 | uint32_t(image[source]) << 16 |
                                 uint32_t(image[source + 1]) << 8 | image[source + 2];
        }
    auto *surface = cairo_image_surface_create_for_data(
        reinterpret_cast<unsigned char *>(argb.data()), CAIRO_FORMAT_ARGB32, Size, Size, Size * 4);
    assert(cairo_surface_status(surface) == CAIRO_STATUS_SUCCESS);
    const auto status = cairo_surface_write_to_png(surface, path.c_str());
    cairo_surface_destroy(surface);
    assert(status == CAIRO_STATUS_SUCCESS);
}
Pixels mask(ModelRenderer &gpu) {
    const char *vertex = R"(#version 330 core
        layout(location=0) in vec3 position;
        layout(location=4) in float touchpad;
        uniform mat4 mvp;
        flat out int pad;
        void main() { gl_Position=mvp*vec4(position,1); pad=int(touchpad); })";
    const char *fragment = R"(#version 330 core
        flat in int pad;
        out vec4 color;
        void main() { color=vec4(pad==1 ? 1 : 0,pad==2 ? 1 : 0,0,1); })";
    const auto vs = ct::ui::model_shader(GL_VERTEX_SHADER, vertex);
    const auto fs = ct::ui::model_shader(GL_FRAGMENT_SHADER, fragment);
    const auto program = glCreateProgram();
    glAttachShader(program, vs);
    glAttachShader(program, fs);
    glLinkProgram(program);
    GLint linked = 0;
    glGetProgramiv(program, GL_LINK_STATUS, &linked);
    assert(linked);
    glDeleteShader(vs);
    glDeleteShader(fs);
    glBindFramebuffer(GL_FRAMEBUFFER, gpu.framebuffers[0]);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    glUseProgram(program);
    const auto mvp = ct::multiply(projection(), model());
    glUniformMatrix4fv(glGetUniformLocation(program, "mvp"), 1, GL_FALSE, mvp.data());
    glDrawArrays(GL_TRIANGLES, 0, gpu.count);
    glFinish();
    auto result = pixels();
    glUseProgram(gpu.program);
    glDeleteProgram(program);
    return result;
}
std::array<double, 2> check_spot(const Pixels &baseline, const Pixels &image, const Pixels &regions,
                                 size_t side) {
    double weight = 0, x = 0, y = 0;
    for (int row = 0; row < Size; ++row)
        for (int column = 0; column < Size; ++column) {
            const size_t at = (row * Size + column) * 4;
            assert(image[at + 3] == baseline[at + 3]);
            if (std::equal(image.begin() + at, image.begin() + at + 4, baseline.begin() + at))
                continue;
            assert(regions[at + side] == 255);
            const int blue = int(image[at + 2]) - baseline[at + 2];
            assert(blue >= 0 && blue >= int(image[at]) - baseline[at]);
            weight += blue;
            x += (column + .5) * blue;
            y += (row + .5) * blue;
        }
    assert(weight > 100);
    return {x / weight, y / weight};
}
void contact(ModelRenderer &gpu, uint32_t buttons, float x = 0, float y = 0) {
    gpu.touchpads = {};
    const float position[3]{x, y, 0};
    gpu.touchpads.update(buttons, position, position, 1, 1, true);
    gpu.touchpads.update(buttons, position, position, 1.04, 1.04, true);
}
void asset_checks(const std::vector<ct::Vertex> &vertices, const std::string &asset) {
    ct::ButtonAnimation animation;
    auto model_vertices = vertices;
    std::istringstream input(asset);
    animation.load(input, model_vertices);
    std::array<unsigned, 3> counts{};
    for (size_t i = 0; i < vertices.size(); ++i) {
        const auto &vertex = model_vertices[i];
        assert(std::equal(vertex.position, vertex.position + 3, vertices[i].position));
        assert(std::equal(vertex.normal, vertex.normal + 3, vertices[i].normal));
        assert(vertex.part < animation.poses.size());
        assert(vertex.touchpad >= 0 && vertex.touchpad <= 2);
        ++counts[size_t(vertex.touchpad)];
        if (vertex.touchpad)
            assert(vertex.part == 0 &&
                   (vertex.touchpad == 1 ? vertex.position[0] < 0 : vertex.position[0] > 0));
    }
    assert(counts[0] && counts[1] == 6813 * 3 && counts[2] == 7208 * 3);
    for (const auto mode : {0, 1, 2, 3, 4, 5, 6}) {
        std::string invalid = asset;
        if (mode == 0)
            invalid[7] = '2';
        if (mode == 1)
            invalid.back() = 22;
        if (mode == 2)
            invalid.resize(invalid.size() - 1);
        if (mode == 3)
            invalid += 'x';
        constexpr size_t mapping_offset = 8 + 4 + 8 + 12 + 14 * 16 + 4 * 12;
        if (mode == 4) {
            const float nonfinite = std::numeric_limits<float>::quiet_NaN();
            std::memcpy(invalid.data() + mapping_offset, &nonfinite, sizeof(nonfinite));
        }
        if (mode == 5)
            std::memset(invalid.data() + mapping_offset + 8, 0, 8);
        if (mode == 6)
            for (size_t i = mapping_offset + 12 * sizeof(float); i < invalid.size(); ++i)
                if (invalid[i] == 20)
                    invalid[i] = 0;
        std::istringstream malformed(invalid);
        bool rejected = false;
        try {
            animation.load(malformed, model_vertices);
        } catch (const std::runtime_error &) {
            rejected = true;
        }
        assert(rejected);
    }
}
} // namespace

int main() {
    state_checks();
    std::ifstream mesh("assets/controller.stl", std::ios::binary);
    auto vertices = ct::read_mesh(mesh);
    std::ifstream controls("assets/controller.buttons", std::ios::binary);
    const std::string asset((std::istreambuf_iterator<char>(controls)), {});
    asset_checks(vertices, asset);
    ModelRenderer gpu;
    std::istringstream input(asset);
    gpu.buttons.load(input, vertices);
    std::ifstream appearance("assets/controller.surface", std::ios::binary);
    const auto surface = ct::read_surface(appearance, vertices);
    gpu.initialize(vertices, false, &surface);
    const auto baseline = render(gpu);
    const auto regions = mask(gpu);
    const auto output = std::filesystem::path("build/touchpad-glow");
    std::filesystem::create_directories(output);
    save(baseline, output / "no-touch.png");
    std::ofstream(output / "no-touch.rgba", std::ios::binary)
        .write(reinterpret_cast<const char *>(baseline.data()), baseline.size());
    for (size_t side = 0; side < 2; ++side) {
        for (const auto point : {std::array<float, 2>{0, 0},
                                 {-.7f, -.7f},
                                 {-.7f, .7f},
                                 {.7f, -.7f},
                                 {.7f, .7f},
                                 {-1, 0},
                                 {1, 0},
                                 {0, -1},
                                 {0, 1},
                                 {-1, -1},
                                 {1, 1}}) {
            contact(gpu, TouchpadGlow::masks[side], point[0], point[1]);
            const auto image = render(gpu);
            const auto centroid = check_spot(baseline, image, regions, side);
            if (std::abs(point[0]) < 1 && std::abs(point[1]) < 1) {
                const auto &mapping = gpu.buttons.pad_mapping[side];
                const auto location =
                    ct::transform(ct::multiply(projection(), model()),
                                  {mapping[0] + point[0] * mapping[2] + point[1] * mapping[4], 0,
                                   mapping[1] + point[0] * mapping[3] + point[1] * mapping[5]});
                assert(std::abs(centroid[0] - (location[0] + 1) * Size / 2) < 8);
                assert(std::abs(centroid[1] - (location[1] + 1) * Size / 2) < 8);
            }
            if (point == std::array<float, 2>{0, 0})
                save(image, output / (side ? "right-touch.png" : "left-touch.png"));
        }
    }
    contact(gpu, TouchpadGlow::masks[0] | TouchpadGlow::masks[1]);
    const auto both = render(gpu);
    save(both, output / "both-touch.png");
    size_t changed[2]{};
    for (size_t at = 0; at < both.size(); at += 4)
        if (!std::equal(both.begin() + at, both.begin() + at + 4, baseline.begin() + at)) {
            assert(regions[at] == 255 || regions[at + 1] == 255);
            ++changed[regions[at] == 255 ? 0 : 1];
        }
    assert(changed[0] > 100 && changed[1] > 100);
    contact(gpu, TouchpadGlow::masks[0] | TouchpadGlow::masks[1], .6f, .6f);
    save(render(gpu), output / "moved-touch.png");
    const float position[3]{};
    for (const double time : {1.08, 1.12, 1.16})
        gpu.touchpads.update(0, position, position, time, time, true);
    assert(render(gpu) == baseline);
    contact(gpu, TouchpadGlow::masks[0]);
    gpu.touchpads.update(0, position, position, 1.04, 1.20, true);
    assert(render(gpu) == baseline);
    glUseProgram(gpu.program);
    glUniform1i(glGetUniformLocation(gpu.program, "has_appearance"), false);
    const auto untextured = render(gpu);
    contact(gpu, TouchpadGlow::masks[1]);
    check_spot(untextured, render(gpu), regions, 1);
    std::cout << "touchpad glow: independent contacts, smoothing, freshness, asset validation, "
                 "GPU mapping, clipping, release and appearance fallback passed\n";
}
