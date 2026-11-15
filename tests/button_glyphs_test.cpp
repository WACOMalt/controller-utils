#include "ui/button_glyphs.h"
#include <cassert>

int main() {
    assert(ButtonGlyphs::binding("View + Menu") == "[View] + [Menu]");
    assert(ButtonGlyphs::binding("R4 + A") == "[R4] + [A]");
    assert(ButtonGlyphs::binding("Unknown") == "Unknown");
    assert(ButtonGlyphs::chords("Hold View + Menu.") == "Hold [View] + [Menu].");
    const auto image = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, 1200, 100);
    const auto c = cairo_create(image);
    cairo_select_font_face(c, "sans", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);
    cairo_set_source_rgb(c, 1, 1, 1);
    ButtonGlyphs glyphs;
    for (const auto &value : {"Hold [View] + [Menu] for 1 second.", "[Steam] + [X]",
                              "[Unknown] and [", "[RightPadClick] [R4] [L2]"}) {
        cairo_save(c);
        cairo_set_operator(c, CAIRO_OPERATOR_CLEAR);
        cairo_paint(c);
        cairo_restore(c);
        const auto bounds = glyphs.layout(c, value, 24, true, 10, 50);
        cairo_surface_flush(image);
        const auto data = cairo_image_surface_get_data(image);
        const auto stride = cairo_image_surface_get_stride(image);
        bool found = false;
        for (int y = 0; y < 100; ++y)
            for (int x = 0; x < 1200; ++x) {
                uint32_t pixel = 0;
                std::memcpy(&pixel, data + y * stride + x * 4, 4);
                if (!(pixel >> 24))
                    continue;
                found = true;
                assert(x >= std::floor(10 + bounds.x_bearing) - 1);
                assert(x <= std::ceil(10 + bounds.x_bearing + bounds.width) + 1);
                assert(y >= std::floor(50 + bounds.y_bearing) - 1);
                assert(y <= std::ceil(50 + bounds.y_bearing + bounds.height) + 1);
            }
        assert(found && bounds.x_advance > 0 && bounds.x_advance < 1180);
    }
    const char *home = std::getenv("HOME");
    const std::string saved_home = home ? home : "";
    unsetenv("HOME");
    ButtonGlyphs missing;
    const auto fallback = missing.layout(c, "[View] + [Menu]", 24);
    cairo_text_extents_t plain{};
    cairo_text_extents(c, "View + Menu", &plain);
    assert(std::abs(fallback.x_advance - plain.x_advance) < .01);
    if (home)
        setenv("HOME", saved_home.c_str(), 1);
    cairo_destroy(c);
    cairo_surface_destroy(image);
}
