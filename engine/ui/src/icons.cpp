#include "slash/ui/ui.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <map>
#include <string>
#include <string_view>
#include <vector>

// The icons of Godot's default theme (scene/theme/icons/*.svg), drawn here from the same shapes:
// rounded rects, circles, rings, a check mark and stroked lines, each in one color. Pixels are
// covered by 8x8 samples each, with only exactly rounded math, so every machine draws them alike.
namespace slash::ui {
namespace {

constexpr int kSamples = 8; // per pixel, each way

struct Shape {
    enum class Kind : std::uint8_t { Rect, Circle, Ring, Polygon, Stroke };
    Kind kind = Kind::Rect;
    // Rect: x, y, width, height and corner radius. Circle and ring: center x, y, radius and,
    // for a ring, inner radius. Stroke: half the line's width.
    std::array<float, 5> v{};
    std::vector<Vec2> points; // Polygon; Stroke, the line's points, with round caps and joins
    Vec4 color;               // straight, its alpha the SVG's fill-opacity
};

// #1a1a1a and white, as the icons fill with them.
constexpr float kDark = 26.0F / 255.0F;
constexpr float kLight = 254.0F / 255.0F; // the slider grabbers' #fefefe

Vec4 dark(float alpha) {
    return {kDark, kDark, kDark, alpha};
}

Vec4 white(float alpha) {
    return {1.0F, 1.0F, 1.0F, alpha};
}

Shape rect(float x, float y, float width, float height, float radius, Vec4 color) {
    return {.kind = Shape::Kind::Rect,
            .v = {x, y, width, height, radius},
            .points = {},
            .color = color};
}

Shape circle(float x, float y, float radius, Vec4 color) {
    return {
        .kind = Shape::Kind::Circle, .v = {x, y, radius, 0.0F, 0.0F}, .points = {}, .color = color};
}

bool inside(const Shape& shape, float x, float y) {
    const auto& v = shape.v;
    switch (shape.kind) {
    case Shape::Kind::Rect: {
        if (x < v[0] || y < v[1] || x > v[0] + v[2] || y > v[1] + v[3]) {
            return false;
        }
        const float r = v[4];
        const float dx = x - std::clamp(x, v[0] + r, v[0] + v[2] - r);
        const float dy = y - std::clamp(y, v[1] + r, v[1] + v[3] - r);
        return (dx * dx) + (dy * dy) <= r * r;
    }
    case Shape::Kind::Circle:
    case Shape::Kind::Ring: {
        const float dx = x - v[0];
        const float dy = y - v[1];
        const float d = (dx * dx) + (dy * dy);
        return d <= v[2] * v[2] && (shape.kind == Shape::Kind::Circle || d >= v[3] * v[3]);
    }
    case Shape::Kind::Polygon: {
        // Even-odd, as a ray to the right crosses the edges.
        bool in = false;
        const std::vector<Vec2>& p = shape.points;
        for (std::size_t i = 0, j = p.size() - 1; i < p.size(); j = i++) {
            if ((p[i].y > y) != (p[j].y > y) &&
                x < ((p[j].x - p[i].x) * (y - p[i].y) / (p[j].y - p[i].y)) + p[i].x) {
                in = !in;
            }
        }
        return in;
    }
    case Shape::Kind::Stroke: {
        // Within half the width of a segment.
        const std::vector<Vec2>& p = shape.points;
        for (std::size_t i = 0; i + 1 < p.size(); ++i) {
            const float ex = p[i + 1].x - p[i].x;
            const float ey = p[i + 1].y - p[i].y;
            const float length = (ex * ex) + (ey * ey);
            const float along =
                length > 0.0F
                    ? std::clamp((((x - p[i].x) * ex) + ((y - p[i].y) * ey)) / length, 0.0F, 1.0F)
                    : 0.0F;
            const float dx = x - (p[i].x + (along * ex));
            const float dy = y - (p[i].y + (along * ey));
            if ((dx * dx) + (dy * dy) <= v[0] * v[0]) {
                return true;
            }
        }
        return false;
    }
    }
    return false;
}

// Draws shapes in order over a clear image, as an SVG renderer composites them.
render::Image draw(std::uint32_t width, std::uint32_t height, const std::vector<Shape>& shapes) {
    render::Image image{.width = width, .height = height, .pixels = {}};
    image.pixels.resize(std::size_t{width} * height * 4);
    constexpr float kStep = 1.0F / static_cast<float>(kSamples);
    for (std::uint32_t py = 0; py < height; ++py) {
        for (std::uint32_t px = 0; px < width; ++px) {
            // Premultiplied while compositing.
            std::array<float, 4> out{};
            for (const Shape& shape : shapes) {
                int covered = 0;
                for (int sy = 0; sy < kSamples; ++sy) {
                    const float y =
                        static_cast<float>(py) + ((static_cast<float>(sy) + 0.5F) * kStep);
                    for (int sx = 0; sx < kSamples; ++sx) {
                        const float x =
                            static_cast<float>(px) + ((static_cast<float>(sx) + 0.5F) * kStep);
                        covered += inside(shape, x, y) ? 1 : 0;
                    }
                }
                const float a = shape.color.w * static_cast<float>(covered) /
                                static_cast<float>(kSamples * kSamples);
                out[0] = (shape.color.x * a) + (out[0] * (1.0F - a));
                out[1] = (shape.color.y * a) + (out[1] * (1.0F - a));
                out[2] = (shape.color.z * a) + (out[2] * (1.0F - a));
                out[3] = a + (out[3] * (1.0F - a));
            }
            const std::size_t at = ((std::size_t{py} * width) + px) * 4;
            const auto byte = [](float value) {
                return static_cast<std::uint8_t>(
                    std::lround(std::clamp(value, 0.0F, 1.0F) * 255.0F));
            };
            for (std::size_t c = 0; c < 3; ++c) {
                image.pixels[at + c] = out[3] > 0.0F ? byte(out.at(c) / out[3]) : 0;
            }
            image.pixels[at + 3] = byte(out[3]);
        }
    }
    return image;
}

const std::map<std::string, render::Image, std::less<>>& icons() {
    static const std::map<std::string, render::Image, std::less<>> kIcons = [] {
        std::map<std::string, render::Image, std::less<>> out;
        // checked.svg's check mark.
        const Shape check{.kind = Shape::Kind::Polygon,
                          .v = {},
                          .points = {{11.5F, 3.75F},
                                     {5.9F, 9.35F},
                                     {4.2F, 7.65F},
                                     {2.7F, 9.15F},
                                     {5.9F, 12.35F},
                                     {13.0F, 5.25F}},
                          .color = dark(1.0F)};
        Shape check_disabled = check;
        check_disabled.color = dark(0.5F);
        const auto box = [](Vec4 color) { return rect(1.0F, 1.0F, 14.0F, 14.0F, 2.333F, color); };
        out.emplace("checked", draw(16, 16, {box(white(0.75F)), check}));
        out.emplace("checked_disabled", draw(16, 16, {box(white(0.37F)), check_disabled}));
        out.emplace("unchecked", draw(16, 16, {box(dark(0.5F))}));
        out.emplace("unchecked_disabled", draw(16, 16, {box(dark(0.25F))}));
        // radio_checked.svg: a dark dot in a white ring, its stroke 3 wide on a circle of 5.5.
        const Shape ring{.kind = Shape::Kind::Ring,
                         .v = {8.0F, 8.0F, 7.0F, 4.0F, 0.0F},
                         .points = {},
                         .color = white(0.75F)};
        out.emplace("radio_checked", draw(16, 16, {circle(8.0F, 8.0F, 5.5F, dark(1.0F)), ring}));
        out.emplace("radio_checked_disabled", draw(16, 16,
                                                   {circle(8.0F, 8.0F, 7.0F, white(0.37F)),
                                                    circle(8.0F, 8.0F, 4.0F, dark(0.5F))}));
        out.emplace("radio_unchecked", draw(16, 16, {circle(8.0F, 8.0F, 7.0F, dark(0.5F))}));
        out.emplace("radio_unchecked_disabled",
                    draw(16, 16, {circle(8.0F, 8.0F, 7.0F, dark(0.25F))}));
        // The toggle switches: a track with a knob at the end that is on.
        const auto toggle = [](Vec4 track, float knob_x, Vec4 knob) {
            return draw(
                32, 16,
                {rect(1.0F, 1.0F, 30.0F, 14.0F, 7.0F, track), circle(knob_x, 8.0F, 5.0F, knob)});
        };
        out.emplace("toggle_on", toggle(white(0.75F), 24.0F, dark(1.0F)));
        out.emplace("toggle_on_disabled", toggle(white(0.37F), 24.0F, dark(0.5F)));
        out.emplace("toggle_on_mirrored", toggle(white(0.75F), 8.0F, dark(1.0F)));
        out.emplace("toggle_on_disabled_mirrored", toggle(white(0.37F), 8.0F, dark(0.5F)));
        out.emplace("toggle_off", toggle(dark(0.5F), 8.0F, white(0.5F)));
        out.emplace("toggle_off_disabled", toggle(dark(0.25F), 8.0F, white(0.25F)));
        out.emplace("toggle_off_mirrored", toggle(dark(0.5F), 24.0F, white(0.5F)));
        out.emplace("toggle_off_disabled_mirrored", toggle(dark(0.25F), 24.0F, white(0.25F)));
        // Slider grabbers and ticks.
        const auto grabber = [](float alpha) {
            return draw(16, 16, {circle(8.0F, 8.0F, 7.0F, {kLight, kLight, kLight, alpha})});
        };
        out.emplace("slider_grabber", grabber(0.75F));
        out.emplace("slider_grabber_hl", grabber(1.0F));
        out.emplace("slider_grabber_disabled", grabber(0.37F));
        out.emplace("hslider_tick", draw(4, 8, {rect(1.0F, 0.0F, 2.0F, 8.0F, 0.0F, white(0.25F))}));
        out.emplace("vslider_tick", draw(8, 4, {rect(0.0F, 1.0F, 8.0F, 2.0F, 0.0F, white(0.25F))}));
        // option_button_arrow.svg: a chevron stroked 2 wide in #b2b2b2 at 0.85.
        constexpr float kArrow = 178.0F / 255.0F;
        out.emplace("option_button_arrow",
                    draw(12, 12,
                         {{.kind = Shape::Kind::Stroke,
                           .v = {1.0F, 0.0F, 0.0F, 0.0F, 0.0F},
                           .points = {{2.0F, 4.0F}, {6.0F, 8.0F}, {10.0F, 4.0F}},
                           .color = {kArrow, kArrow, kArrow, 0.85F}}}));
        return out;
    }();
    return kIcons;
}

} // namespace

const render::Image* builtin_icon(std::string_view path) {
    if (!path.starts_with(kBuiltinPrefix)) {
        return nullptr;
    }
    const auto found = icons().find(path.substr(kBuiltinPrefix.size()));
    return found != icons().end() ? &found->second : nullptr;
}

const std::vector<std::string_view>& builtin_icon_names() {
    static const std::vector<std::string_view> kNames = [] {
        std::vector<std::string_view> names;
        for (const auto& [name, image] : icons()) {
            names.emplace_back(name);
        }
        return names;
    }();
    return kNames;
}

} // namespace slash::ui
