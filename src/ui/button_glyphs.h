#pragma once
#include <cairo/cairo.h>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <map>
#include <memory>
#include <string>

struct ButtonGlyphs {
    struct Mapping {
        const char *token;
        const char *label;
        const char *file;
    };
    static constexpr Mapping buttons[] = {
        {"View", "View", "sd_button_view"},
        {"Menu", "Menu", "sd_button_menu"},
        {"Steam", "Steam", "sd_button_steam"},
        {"QuickAccess", "Quick Access", "sd_button_aux"},
        {"A", "A", "shared_button_a"},
        {"B", "B", "shared_button_b"},
        {"X", "X", "shared_button_x"},
        {"Y", "Y", "shared_button_y"},
        {"L1", "L1", "sd_l1"},
        {"R1", "R1", "sd_r1"},
        {"L2", "L2", "sd_l2"},
        {"R2", "R2", "sd_r2"},
        {"L3", "L3", "shared_lstick_click"},
        {"R3", "R3", "shared_rstick_click"},
        {"L4", "L4", "sd_l4"},
        {"R4", "R4", "sd_r4"},
        {"L5", "L5", "sd_l5"},
        {"R5", "R5", "sd_r5"},
        {"DpadUp", "D-pad up", "shared_dpad_up"},
        {"DpadDown", "D-pad down", "shared_dpad_down"},
        {"DpadLeft", "D-pad left", "shared_dpad_left"},
        {"DpadRight", "D-pad right", "shared_dpad_right"},
        {"LeftPad", "Left pad", "sd_ltrackpad"},
        {"RightPad", "Right pad", "sd_rtrackpad"},
        {"LeftPadClick", "Left pad press", "sd_ltrackpad_click"},
        {"RightPadClick", "Right pad press", "sd_rtrackpad_click"},
        {"LeftStick", "Left stick", "shared_lstick"},
        {"RightStick", "Right stick", "shared_rstick"}};
    struct Glyph {
        std::unique_ptr<cairo_surface_t, decltype(&cairo_surface_destroy)> image{
            nullptr, cairo_surface_destroy};
        int x = 0, y = 0, width = 0, height = 0;
    };
    std::map<std::string, Glyph> cache;

    static ButtonGlyphs &instance() {
        static ButtonGlyphs glyphs;
        return glyphs;
    }
    static std::string binding(const std::string &label) {
        std::string result;
        size_t start = 0;
        while (start < label.size()) {
            const auto end = label.find(" + ", start);
            const auto part = label.substr(start, end == std::string::npos ? end : end - start);
            std::string token = part;
            for (const auto &button : buttons)
                if (part == button.label) {
                    token = "[" + std::string(button.token) + "]";
                    break;
                }
            result += (result.empty() ? "" : " + ") + token;
            if (end == std::string::npos)
                break;
            start = end + 3;
        }
        return result;
    }
    static std::string chords(std::string value) {
        size_t at = 0;
        while ((at = value.find("View + Menu", at)) != std::string::npos) {
            value.replace(at, 11, "[View] + [Menu]");
            at += 15;
        }
        return value;
    }
    Glyph *load(const Mapping &button, bool dark) {
        const std::string relative =
            std::string(dark ? "dark/" : "light/") + button.file + "_md.png";
        auto [item, inserted] = cache.try_emplace(relative);
        auto &glyph = item->second;
        if (!inserted)
            return glyph.image ? &glyph : nullptr;
        const char *home = std::getenv("HOME");
        if (!home)
            return nullptr;
        for (const auto &root :
             {std::filesystem::path(home) / ".local/share/Steam/controller_base/images/api",
              std::filesystem::path(home) / ".steam/steam/controller_base/images/api"}) {
            std::unique_ptr<cairo_surface_t, decltype(&cairo_surface_destroy)> image(
                cairo_image_surface_create_from_png((root / relative).c_str()),
                cairo_surface_destroy);
            if (cairo_surface_status(image.get()) != CAIRO_STATUS_SUCCESS)
                continue;
            cairo_surface_flush(image.get());
            const auto format = cairo_image_surface_get_format(image.get());
            if (format != CAIRO_FORMAT_ARGB32 && format != CAIRO_FORMAT_RGB24)
                continue;
            const int width = cairo_image_surface_get_width(image.get()),
                      height = cairo_image_surface_get_height(image.get());
            if (width <= 0 || height <= 0 || width > 512 || height > 512)
                continue;
            const auto data = cairo_image_surface_get_data(image.get());
            const auto stride = cairo_image_surface_get_stride(image.get());
            int left = width, top = height, right = -1, bottom = -1;
            for (int y = 0; y < height; ++y)
                for (int x = 0; x < width; ++x) {
                    uint32_t pixel = 0;
                    std::memcpy(&pixel, data + y * stride + x * 4, 4);
                    if (format == CAIRO_FORMAT_RGB24 || pixel >> 24) {
                        left = std::min(left, x);
                        top = std::min(top, y);
                        right = std::max(right, x);
                        bottom = std::max(bottom, y);
                    }
                }
            if (right < left || bottom < top)
                continue;
            glyph.x = left;
            glyph.y = top;
            glyph.width = right - left + 1;
            glyph.height = bottom - top + 1;
            glyph.image = std::move(image);
            return &glyph;
        }
        return nullptr;
    }
    cairo_text_extents_t layout(cairo_t *c, const std::string &value, double size,
                                bool draw = false, double x = 0, double y = 0,
                                uint32_t ink = 0xdcdedf) {
        cairo_set_font_size(c, size);
        cairo_text_extents_t result{};
        double advance = 0, left = 0, top = 0, right = 0, bottom = 0;
        bool bounds = false;
        const auto include = [&](double bx, double by, double w, double h) {
            if (w <= 0 || h <= 0)
                return;
            if (!bounds) {
                left = bx;
                top = by;
                right = bx + w;
                bottom = by + h;
                bounds = true;
            } else {
                left = std::min(left, bx);
                top = std::min(top, by);
                right = std::max(right, bx + w);
                bottom = std::max(bottom, by + h);
            }
        };
        const auto text = [&](const std::string &part) {
            cairo_text_extents_t box{};
            cairo_text_extents(c, part.c_str(), &box);
            include(advance + box.x_bearing, box.y_bearing, box.width, box.height);
            if (draw) {
                cairo_move_to(c, x + advance, y);
                cairo_show_text(c, part.c_str());
            }
            advance += box.x_advance;
        };
        size_t cursor = 0;
        while (cursor < value.size()) {
            const auto open = value.find('[', cursor);
            if (open == std::string::npos) {
                text(value.substr(cursor));
                break;
            }
            text(value.substr(cursor, open - cursor));
            const auto close = value.find(']', open + 1);
            if (close == std::string::npos) {
                text(value.substr(open));
                break;
            }
            const auto token = value.substr(open + 1, close - open - 1);
            const Mapping *mapping = nullptr;
            for (const auto &button : buttons)
                if (token == button.token) {
                    mapping = &button;
                    break;
                }
            auto glyph = mapping ? load(*mapping, ink == 0x0e141b || ink == 0x3d4450) : nullptr;
            if (glyph) {
                const double height = size * 1.1, scale = height / glyph->height,
                             width = scale * glyph->width, by = -size * .9;
                include(advance, by, width, height);
                if (draw) {
                    cairo_save(c);
                    cairo_translate(c, x + advance, y + by);
                    cairo_scale(c, scale, scale);
                    cairo_rectangle(c, 0, 0, glyph->width, glyph->height);
                    cairo_clip(c);
                    cairo_set_source_surface(c, glyph->image.get(), -glyph->x, -glyph->y);
                    cairo_paint(c);
                    cairo_restore(c);
                }
                advance += width;
            } else
                text(mapping ? mapping->label : value.substr(open, close - open + 1));
            cursor = close + 1;
        }
        result.x_bearing = left;
        result.y_bearing = top;
        result.width = right - left;
        result.height = bottom - top;
        result.x_advance = advance;
        return result;
    }
};
