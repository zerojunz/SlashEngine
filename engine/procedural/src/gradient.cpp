#include "slash/procedural/gradient.hpp"

#include "json_fields.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <string>
#include <utility>

namespace slash::procedural {
namespace {

using detail::EnumName;

constexpr std::array<EnumName<GradientInterpolation>, 3> kModes{{
    {GradientInterpolation::Linear, "linear"},
    {GradientInterpolation::Constant, "constant"},
    {GradientInterpolation::Cubic, "cubic"},
}};
constexpr std::array<EnumName<GradientColorSpace>, 3> kSpaces{{
    {GradientColorSpace::Srgb, "srgb"},
    {GradientColorSpace::LinearSrgb, "linear_srgb"},
    {GradientColorSpace::Oklab, "oklab"},
}};

// x to the power y for x above 0, as powf but the same bits everywhere.
float power(float x, float y) {
    return x <= 0.0F ? 0.0F : slash::exp(y * slash::ln(x));
}

// The cube root, as cbrtf but the same bits everywhere.
float cube_root(float x) {
    if (x == 0.0F) {
        return 0.0F;
    }
    const float root = slash::exp(slash::ln(x < 0.0F ? -x : x) / 3.0F);
    return x < 0.0F ? -root : root;
}

float component(const Vec4& color, int i) {
    switch (i) {
    case 0:
        return color.x;
    case 1:
        return color.y;
    case 2:
        return color.z;
    default:
        return color.w;
    }
}

// Godot's Math::cubic_interpolate.
float cubic(float from, float to, float pre, float post, float weight) {
    return 0.5F * ((from * 2.0F) + (-pre + to) * weight +
                   (2.0F * pre - 5.0F * from + 4.0F * to - post) * (weight * weight) +
                   (-pre + 3.0F * from - 3.0F * to + post) * (weight * weight * weight));
}

// Gradient::transform_color_space.
Vec4 to_space(GradientColorSpace space, Vec4 color) {
    switch (space) {
    case GradientColorSpace::LinearSrgb:
        return srgb_to_linear(color);
    case GradientColorSpace::Oklab:
        return linear_to_oklab(srgb_to_linear(color));
    case GradientColorSpace::Srgb:
        break;
    }
    return color;
}

// Gradient::inv_transform_color_space.
Vec4 from_space(GradientColorSpace space, Vec4 color) {
    switch (space) {
    case GradientColorSpace::LinearSrgb:
        return linear_to_srgb(color);
    case GradientColorSpace::Oklab:
        return linear_to_srgb(oklab_to_linear(color));
    case GradientColorSpace::Srgb:
        break;
    }
    return color;
}

// Godot's Math::lerp.
float lerp(float from, float to, float weight) {
    return from + (to - from) * weight;
}

} // namespace

Vec4 srgb_to_linear(Vec4 c) {
    const auto channel = [](float v) {
        return v < 0.04045F ? v * (1.0F / 12.92F)
                            : power(static_cast<float>((v + 0.055) * (1.0 / (1.0 + 0.055))), 2.4F);
    };
    return {channel(c.x), channel(c.y), channel(c.z), c.w};
}

Vec4 linear_to_srgb(Vec4 c) {
    const auto channel = [](float v) {
        return v < 0.0031308F ? 12.92F * v
                              : static_cast<float>((1.0 + 0.055) * power(v, 1.0F / 2.4F) - 0.055);
    };
    return {channel(c.x), channel(c.y), channel(c.z), c.w};
}

Vec4 linear_to_oklab(Vec4 c) {
    const float l = 0.4122214708F * c.x + 0.5363325363F * c.y + 0.0514459929F * c.z;
    const float m = 0.2119034982F * c.x + 0.6806995451F * c.y + 0.1073969566F * c.z;
    const float s = 0.0883024619F * c.x + 0.2817188376F * c.y + 0.6299787005F * c.z;
    const float l_ = cube_root(l);
    const float m_ = cube_root(m);
    const float s_ = cube_root(s);
    return {0.2104542553F * l_ + 0.7936177850F * m_ - 0.0040720468F * s_,
            1.9779984951F * l_ - 2.4285922050F * m_ + 0.4505937099F * s_,
            0.0259040371F * l_ + 0.7827717662F * m_ - 0.8086757660F * s_, c.w};
}

Vec4 oklab_to_linear(Vec4 c) {
    const float l_ = c.x + 0.3963377774F * c.y + 0.2158037573F * c.z;
    const float m_ = c.x - 0.1055613458F * c.y - 0.0638541728F * c.z;
    const float s_ = c.x - 0.0894841775F * c.y - 1.2914855480F * c.z;
    const float l = l_ * l_ * l_;
    const float m = m_ * m_ * m_;
    const float s = s_ * s_ * s_;
    return {+4.0767416621F * l - 3.3077115913F * m + 0.2309699292F * s,
            -1.2684380046F * l + 2.6097574011F * m - 0.3413193965F * s,
            -0.0041960863F * l - 0.7034186147F * m + 1.7076147010F * s, c.w};
}

// Godot's Gradient::get_color_at_offset, line for line.
Vec4 Gradient::color_at(float offset) const {
    if (points.empty()) {
        return {0.0F, 0.0F, 0.0F, 1.0F};
    }
    // Binary search.
    int low = 0;
    int high = static_cast<int>(points.size()) - 1;
    int middle = 0;
    while (low <= high) {
        middle = (low + high) / 2;
        const GradientStop& point = points[static_cast<std::size_t>(middle)];
        if (point.offset > offset) {
            high = middle - 1;
        } else if (point.offset < offset) {
            low = middle + 1;
        } else {
            return point.color;
        }
    }
    if (points[static_cast<std::size_t>(middle)].offset > offset) {
        middle--;
    }
    const int first = middle;
    const int second = middle + 1;
    if (second >= static_cast<int>(points.size())) {
        return points.back().color;
    }
    if (first < 0) {
        return points.front().color;
    }
    const GradientStop& point1 = points[static_cast<std::size_t>(first)];
    const GradientStop& point2 = points[static_cast<std::size_t>(second)];
    const float weight = (offset - point1.offset) / (point2.offset - point1.offset);
    const GradientColorSpace space = interpolation_color_space;
    switch (interpolation_mode) {
    case GradientInterpolation::Constant:
        return point1.color;
    case GradientInterpolation::Cubic: {
        int p0 = first - 1;
        int p3 = second + 1;
        if (p3 >= static_cast<int>(points.size())) {
            p3 = second;
        }
        if (p0 < 0) {
            p0 = first;
        }
        const Vec4 color0 = to_space(space, points[static_cast<std::size_t>(p0)].color);
        const Vec4 color1 = to_space(space, point1.color);
        const Vec4 color2 = to_space(space, point2.color);
        const Vec4 color3 = to_space(space, points[static_cast<std::size_t>(p3)].color);
        std::array<float, 4> out{};
        for (int i = 0; i < 4; ++i) {
            out[static_cast<std::size_t>(i)] =
                cubic(component(color1, i), component(color2, i), component(color0, i),
                      component(color3, i), weight);
        }
        return from_space(space, {out[0], out[1], out[2], out[3]});
    }
    case GradientInterpolation::Linear:
        break;
    }
    const Vec4 color1 = to_space(space, point1.color);
    const Vec4 color2 = to_space(space, point2.color);
    return from_space(space, {lerp(color1.x, color2.x, weight), lerp(color1.y, color2.y, weight),
                              lerp(color1.z, color2.z, weight), lerp(color1.w, color2.w, weight)});
}

json::Value gradient_schema() {
    json::Value color;
    color["type"] = "array";
    color["items"]["type"] = "number";
    color["minItems"] = 3;
    color["maxItems"] = 4;
    color["description"] = "sRGB [r, g, b] or [r, g, b, a], each 0 to 1.";

    json::Value point;
    point["type"] = "object";
    point["additionalProperties"] = false;
    point["required"] = json::Array{"offset", "color"};
    point["properties"]["offset"]["type"] = "number";
    point["properties"]["offset"]["minimum"] = 0;
    point["properties"]["offset"]["maximum"] = 1;
    point["properties"]["offset"]["description"] = "Where along the gradient, 0 to 1.";
    point["properties"]["color"] = color;

    json::Value schema;
    schema["type"] = "object";
    schema["additionalProperties"] = false;
    schema["description"] = "A gradient, as Godot's Gradient. Left out, black to white.";
    json::Value& p = schema["properties"];
    p["points"]["type"] = "array";
    p["points"]["items"] = point;
    p["points"]["description"] = "Colors at offsets. Default: black at 0, white at 1.";
    p["interpolation_mode"] = detail::enum_field(kModes, GradientInterpolation::Linear,
                                                 "How colors blend between points.");
    p["interpolation_color_space"] =
        detail::enum_field(kSpaces, GradientColorSpace::Srgb, "The color space colors blend in.");
    return schema;
}

std::vector<json::ValidationError> validate_gradient(const json::Value& doc,
                                                     const std::string& at) {
    std::vector<json::ValidationError> errors = json::validate(doc, gradient_schema());
    for (json::ValidationError& error : errors) {
        error.path = at + error.path;
    }
    return errors;
}

Result<Gradient> parse_gradient(const json::Value& doc) {
    if (const auto errors = validate_gradient(doc); !errors.empty()) {
        return detail::first_error(errors);
    }
    Gradient gradient;
    if (const json::Value* points = doc.find("points"); points != nullptr) {
        gradient.points.clear();
        for (const json::Value& item : points->as_array()) {
            const json::Array& rgba = item.find("color")->as_array();
            GradientStop stop;
            stop.offset = static_cast<float>(item.find("offset")->as_double());
            stop.color = {static_cast<float>(rgba[0].as_double()),
                          static_cast<float>(rgba[1].as_double()),
                          static_cast<float>(rgba[2].as_double()),
                          rgba.size() > 3 ? static_cast<float>(rgba[3].as_double()) : 1.0F};
            gradient.points.push_back(stop);
        }
        std::ranges::stable_sort(gradient.points, {}, &GradientStop::offset);
    }
    detail::read(doc, "interpolation_mode", kModes, gradient.interpolation_mode);
    detail::read(doc, "interpolation_color_space", kSpaces, gradient.interpolation_color_space);
    return gradient;
}

json::Value gradient_to_json(const Gradient& gradient) {
    json::Value out = json::Object{};
    const Gradient fallback;
    const auto same = [](const GradientStop& a, const GradientStop& b) {
        return a.offset == b.offset && a.color == b.color;
    };
    if (!std::ranges::equal(gradient.points, fallback.points, same)) {
        json::Array points;
        for (const GradientStop& stop : gradient.points) {
            json::Value point;
            point["offset"] = stop.offset;
            point["color"] = json::Array{stop.color.x, stop.color.y, stop.color.z, stop.color.w};
            points.push_back(std::move(point));
        }
        out["points"] = std::move(points);
    }
    detail::write(out, "interpolation_mode", kModes, gradient.interpolation_mode,
                  fallback.interpolation_mode);
    detail::write(out, "interpolation_color_space", kSpaces, gradient.interpolation_color_space,
                  fallback.interpolation_color_space);
    return out;
}

} // namespace slash::procedural
