#include "slash/procedural/texture.hpp"

#include "json_fields.hpp"

#include "slash/asset/assets.hpp"
#include "slash/render/image.hpp"

#include <algorithm>
#include <array>
#include <cfloat>
#include <cmath>
#include <cstddef>
#include <string>
#include <utility>

namespace slash::procedural {
namespace {

using detail::EnumName;

// The largest texture every GPU takes, as render::decode_image allows.
constexpr std::int64_t kMaxSize = 16384;

constexpr std::string_view kNoiseType = "NoiseTexture2D";
constexpr std::string_view kGradient1DType = "GradientTexture1D";
constexpr std::string_view kGradient2DType = "GradientTexture2D";

constexpr std::array<EnumName<GradientFill>, 3> kFills{{
    {GradientFill::Linear, "linear"},
    {GradientFill::Radial, "radial"},
    {GradientFill::Square, "square"},
}};
constexpr std::array<EnumName<GradientRepeat>, 3> kRepeats{{
    {GradientRepeat::None, "none"},
    {GradientRepeat::Repeat, "repeat"},
    {GradientRepeat::Mirror, "mirror"},
}};

// ---- Schemas ----

json::Value base_schema(std::string_view type, std::string_view description) {
    json::Value schema;
    schema["type"] = "object";
    schema["additionalProperties"] = false;
    schema["required"] = json::Array{"format", "type"};
    schema["description"] = description;
    json::Value& p = schema["properties"];
    p["format"]["const"] = 1;
    p["format"]["description"] = "Format version. Must be 1.";
    p["type"]["const"] = type;
    p["type"]["description"] = "Which of Godot's texture resources this is.";
    return schema;
}

json::Value size_field(std::int64_t fallback, std::string_view description) {
    return detail::integer_field(fallback, description, 1, kMaxSize);
}

json::Value point_field(Vec2 fallback, std::string_view description) {
    json::Value field;
    field["type"] = "array";
    field["items"]["type"] = "number";
    field["minItems"] = 2;
    field["maxItems"] = 2;
    field["default"] = json::Array{fallback.x, fallback.y};
    field["description"] = description;
    return field;
}

json::Value noise_texture_schema() {
    const NoiseTexture2D d;
    json::Value schema = base_schema(kNoiseType, "Noise drawn into a texture, as Godot's "
                                                 "NoiseTexture2D.");
    json::Value& p = schema["properties"];
    p["width"] = size_field(d.width, "Width in pixels.");
    p["height"] = size_field(d.height, "Height in pixels.");
    p["invert"] = detail::boolean_field(d.invert, "Turn light into dark and dark into light.");
    p["in_3d_space"] = detail::boolean_field(d.in_3d_space, "Sample 3D noise at z 0.");
    p["seamless"] = detail::boolean_field(d.seamless, "Make the texture tile without seams.");
    p["seamless_blend_skirt"] = detail::number_field(
        d.seamless_blend_skirt, "Share of the texture blended across the seams.", 0.05, 1.0);
    p["as_normal_map"] =
        detail::boolean_field(d.as_normal_map, "Make a normal map of the noise as a bump map.");
    p["bump_strength"] = detail::number_field(d.bump_strength, "Strength of the bumps.");
    p["normalize"] = detail::boolean_field(
        d.normalize, "Stretch the values over the full range; else map -1 to 1 onto it.");
    p["color_ramp"] = gradient_schema();
    p["color_ramp"]["description"] =
        "Colors each value maps to, as a gradient from 0 (dark) to 1 (light).";
    p["noise"] = noise_schema();
    return schema;
}

json::Value gradient_1d_schema() {
    const GradientTexture1D d;
    json::Value schema = base_schema(kGradient1DType, "A gradient across a texture one pixel "
                                                      "high, as Godot's GradientTexture1D.");
    json::Value& p = schema["properties"];
    p["width"] = size_field(d.width, "Width in pixels.");
    p["gradient"] = gradient_schema();
    return schema;
}

json::Value gradient_2d_schema() {
    const GradientTexture2D d;
    json::Value schema =
        base_schema(kGradient2DType, "A gradient filling a texture, as Godot's GradientTexture2D.");
    json::Value& p = schema["properties"];
    p["width"] = size_field(d.width, "Width in pixels.");
    p["height"] = size_field(d.height, "Height in pixels.");
    p["fill"] = detail::enum_field(
        kFills, d.fill, "Along a line, out from a point, or out from a point as squares.");
    p["fill_from"] = point_field(d.fill_from, "Where offset 0 is, in UV (0 to 1 across).");
    p["fill_to"] = point_field(d.fill_to, "Where offset 1 is, in UV.");
    p["repeat"] = detail::enum_field(kRepeats, d.repeat,
                                     "Past fill_to: hold the last color, repeat or mirror.");
    p["gradient"] = gradient_schema();
    return schema;
}

// ---- Noise images (modules/noise/noise.cpp) ----

// Noise::_alpha_blend<uint8_t>.
std::uint8_t alpha_blend(std::uint8_t bg, std::uint8_t fg, int alpha) {
    const int a = alpha + 1;
    const int inverse = 256 - alpha;
    return static_cast<std::uint8_t>((a * fg + inverse * bg) >> 8);
}

// Math::smoothstep(float, float, float).
float smoothstep(float from, float to, float s) {
    const float t = std::clamp((s - from) / (to - from), 0.0F, 1.0F);
    return t * t * (3.0F - 2.0F * t);
}

// Noise::img_buff: an array indexer with offsets and a second modulo.
struct Buffer {
    std::uint8_t* img = nullptr;
    int width = 0;
    int height = 0;
    int offset_x = 0;
    int offset_y = 0;
    int alt_width = 0;
    int alt_height = 0;

    enum Mode : std::uint8_t { Default, AltX, AltY, AltXY };

    std::uint8_t& operator()(int x, int y, Mode mode = Default) const {
        const int mx = (mode == AltXY || mode == AltX) ? alt_width : width;
        const int my = (mode == AltXY || mode == AltY) ? alt_height : height;
        const int index = (x + offset_x) % mx + ((y + offset_y) % my) * width;
        return img[static_cast<std::size_t>(index)];
    }
};

// Noise::_get_image for one slice.
std::vector<std::uint8_t> gray_image(const Noise& noise, int width, int height, bool invert,
                                     bool in_3d_space, bool normalize) {
    const std::vector<float> values = noise.get_values(width, height, in_3d_space);
    std::vector<std::uint8_t> out(values.size());
    if (normalize) {
        float min_value = FLT_MAX;
        float max_value = -FLT_MAX;
        for (const float value : values) {
            max_value = std::max(value, max_value);
            min_value = std::min(value, min_value);
        }
        for (std::size_t i = 0; i < values.size(); ++i) {
            std::uint8_t value = 0;
            if (max_value != min_value) {
                value = static_cast<std::uint8_t>(std::clamp(
                    (values[i] - min_value) / (max_value - min_value) * 255.0F, 0.0F, 255.0F));
            }
            out[i] = invert ? static_cast<std::uint8_t>(255 - value) : value;
        }
        return out;
    }
    for (std::size_t i = 0; i < values.size(); ++i) {
        const auto value =
            static_cast<std::uint8_t>(std::clamp(values[i] * 127.5F + 127.5F, 0.0F, 255.0F));
        out[i] = invert ? static_cast<std::uint8_t>(255 - value) : value;
    }
    return out;
}

// Noise::_generate_seamless_image for one slice: the quadrants of a larger image swapped so the
// edges match, then its skirt blended over the seams through the middle.
std::vector<std::uint8_t> seamless_image(std::vector<std::uint8_t> source, int width, int height,
                                         float blend_skirt) {
    const int skirt_width =
        static_cast<int>(std::max(1.0F, static_cast<float>(width) * blend_skirt));
    const int skirt_height =
        static_cast<int>(std::max(1.0F, static_cast<float>(height) * blend_skirt));
    const int src_width = width + skirt_width;
    const int src_height = height + skirt_height;
    const auto half_width = static_cast<int>(width * 0.5);
    const auto half_height = static_cast<int>(height * 0.5);
    const int skirt_edge_x = half_width + skirt_width;
    const int skirt_edge_y = half_height + skirt_height;

    std::vector<std::uint8_t> dest(static_cast<std::size_t>(width) *
                                   static_cast<std::size_t>(height));
    const Buffer src{source.data(), src_width, src_height, half_width, half_height, width, height};
    const Buffer out{dest.data(), width, height, 0, 0, 0, 0};

    // Swap the quadrants to make the edges seamless.
    for (int y = 0; y < height; y++) {
        for (int x = 0; x < width; x++) {
            out(x, y) = src(x, y, Buffer::AltXY);
        }
    }
    const auto weight = [](int at, int edge, int skirt) {
        return static_cast<int>(
            255 * (1 - smoothstep(0.1F, 0.9F,
                                  static_cast<float>(at - edge) / static_cast<float>(skirt))));
    };
    // Blend the vertical skirt over the middle seam.
    for (int x = half_width; x < skirt_edge_x; x++) {
        const int alpha = weight(x, half_width, skirt_width);
        for (int y = 0; y < height; y++) {
            if (y == half_height) {
                // Skip the center square.
                y = skirt_edge_y - 1;
            } else {
                out(x, y) = alpha_blend(out(x, y), src(x, y, Buffer::AltY), alpha);
            }
        }
    }
    // Blend the horizontal skirt over the middle seam.
    for (int y = half_height; y < skirt_edge_y; y++) {
        const int alpha = weight(y, half_height, skirt_height);
        for (int x = 0; x < width; x++) {
            if (x == half_width) {
                x = skirt_edge_x - 1;
            } else {
                out(x, y) = alpha_blend(out(x, y), src(x, y, Buffer::AltX), alpha);
            }
        }
    }
    // Fill in the center square.
    for (int y = half_height; y < skirt_edge_y; y++) {
        for (int x = half_width; x < skirt_edge_x; x++) {
            const int xpos = weight(x, half_width, skirt_width);
            const int ypos = weight(y, half_height, skirt_height);
            const std::uint8_t top =
                alpha_blend(src(x, y, Buffer::AltX), src(x, y, Buffer::Default), xpos);
            const std::uint8_t bottom =
                alpha_blend(src(x, y, Buffer::AltXY), src(x, y, Buffer::AltY), xpos);
            out(x, y) = alpha_blend(bottom, top, ypos);
        }
    }
    return dest;
}

// Image::_set_color_at_ofs for RGBA8: truncated, as Godot's set_pixel stores it.
std::uint8_t set_byte(float value) {
    return static_cast<std::uint8_t>(std::clamp(static_cast<double>(value) * 255.0, 0.0, 255.0));
}

// Color::get_r8 and its kin: rounded.
std::uint8_t round_byte(float value) {
    return static_cast<std::uint8_t>(std::clamp(std::round(value * 255.0F), 0.0F, 255.0F));
}

render::Image rgba_image(int width, int height) {
    render::Image image;
    image.width = static_cast<std::uint32_t>(width);
    image.height = static_cast<std::uint32_t>(height);
    image.pixels.assign(static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 4, 0);
    return image;
}

void put_color(render::Image& image, std::size_t pixel, Vec4 color, bool rounded) {
    std::uint8_t* out = image.pixels.data() + pixel * 4;
    const auto byte = rounded ? round_byte : set_byte;
    out[0] = byte(color.x);
    out[1] = byte(color.y);
    out[2] = byte(color.z);
    out[3] = byte(color.w);
}

render::Image noise_texture_image(const NoiseTexture2D& texture) {
    const Noise noise(texture.noise);
    const std::vector<std::uint8_t> gray =
        noise_image(noise, texture.width, texture.height, texture.invert, texture.in_3d_space,
                    texture.normalize, texture.seamless, texture.seamless_blend_skirt);
    render::Image image = rgba_image(texture.width, texture.height);
    for (std::size_t i = 0; i < gray.size(); ++i) {
        if (texture.color_ramp) {
            // NoiseTexture2D::_modulate_with_gradient: the gray pixel's luminance picks the color.
            const auto l = static_cast<float>(gray[i] / 255.0);
            const float luminance = 0.2126F * l + 0.7152F * l + 0.0722F * l;
            put_color(image, i, texture.color_ramp->color_at(luminance), false);
        } else {
            std::uint8_t* out = image.pixels.data() + i * 4;
            out[0] = gray[i];
            out[1] = gray[i];
            out[2] = gray[i];
            out[3] = 255;
        }
    }
    if (texture.as_normal_map) {
        bump_to_normal_map(image, texture.bump_strength);
    }
    return image;
}

render::Image gradient_1d_image(const GradientTexture1D& texture) {
    render::Image image = rgba_image(texture.width, 1);
    for (int i = 0; i < texture.width; i++) {
        const float offset = texture.width > 1
                                 ? static_cast<float>(i) / static_cast<float>(texture.width - 1)
                                 : 0.0F;
        put_color(image, static_cast<std::size_t>(i), texture.gradient.color_at(offset), true);
    }
    return image;
}

float length(Vec2 v) {
    return std::sqrt(v.x * v.x + v.y * v.y);
}

float dot(Vec2 a, Vec2 b) {
    return a.x * b.x + a.y * b.y;
}

// GradientTexture2D::_get_gradient_offset_at.
float gradient_offset(const GradientTexture2D& t, int x, int y) {
    if (t.fill_to == t.fill_from) {
        return 0.0F;
    }
    float offset = 0.0F;
    Vec2 pos;
    if (t.width > 1) {
        pos.x = static_cast<float>(x) / static_cast<float>(t.width - 1);
    }
    if (t.height > 1) {
        pos.y = static_cast<float>(y) / static_cast<float>(t.height - 1);
    }
    if (t.fill == GradientFill::Linear) {
        // Geometry2D::get_closest_point_to_segment_uncapped.
        Vec2 closest = t.fill_from;
        const Vec2 p = pos - t.fill_from;
        const Vec2 n = t.fill_to - t.fill_from;
        const float l2 = n.x * n.x + n.y * n.y;
        if (l2 >= 1e-20F) {
            closest = t.fill_from + n * (dot(n, p) / l2);
        }
        offset = length(closest - t.fill_from) / length(t.fill_to - t.fill_from);
        if (dot(closest - t.fill_from, t.fill_to - t.fill_from) < 0) {
            offset *= -1;
        }
    } else if (t.fill == GradientFill::Radial) {
        offset = length(pos - t.fill_from) / length(t.fill_to - t.fill_from);
    } else {
        offset =
            std::max(std::abs(pos.x - t.fill_from.x), std::abs(pos.y - t.fill_from.y)) /
            std::max(std::abs(t.fill_to.x - t.fill_from.x), std::abs(t.fill_to.y - t.fill_from.y));
    }
    if (t.repeat == GradientRepeat::None) {
        offset = static_cast<float>(std::clamp(static_cast<double>(offset), 0.0, 1.0));
    } else if (t.repeat == GradientRepeat::Repeat) {
        offset = std::fmod(offset, 1.0F);
        if (offset < 0) {
            offset = 1 + offset;
        }
    } else {
        offset = std::abs(offset);
        offset = std::fmod(offset, 2.0F);
        if (offset > 1.0) {
            offset = 2.0F - offset;
        }
    }
    return offset;
}

render::Image gradient_2d_image(const GradientTexture2D& texture) {
    render::Image image = rgba_image(texture.width, texture.height);
    const std::size_t count = image.pixels.size() / 4;
    if (texture.gradient.points.size() <= 1) {
        const Vec4 color = texture.gradient.points.empty() ? Vec4{0.0F, 0.0F, 0.0F, 1.0F}
                                                           : texture.gradient.points[0].color;
        for (std::size_t i = 0; i < count; ++i) {
            put_color(image, i, color, false);
        }
        return image;
    }
    for (int y = 0; y < texture.height; y++) {
        for (int x = 0; x < texture.width; x++) {
            const std::size_t pixel =
                static_cast<std::size_t>(x) +
                static_cast<std::size_t>(y) * static_cast<std::size_t>(texture.width);
            put_color(image, pixel, texture.gradient.color_at(gradient_offset(texture, x, y)),
                      true);
        }
    }
    return image;
}

Vec2 read_point(const json::Value& doc, std::string_view key, Vec2 fallback) {
    const json::Value* value = doc.find(key);
    if (value == nullptr) {
        return fallback;
    }
    const json::Array& items = value->as_array();
    return {static_cast<float>(items[0].as_double()), static_cast<float>(items[1].as_double())};
}

void write_point(json::Value& out, std::string_view key, Vec2 value, Vec2 fallback) {
    if (value != fallback) {
        out[key] = json::Array{value.x, value.y};
    }
}

void write_gradient(json::Value& out, std::string_view key, const Gradient& gradient) {
    json::Value written = gradient_to_json(gradient);
    if (written.size() > 0) {
        out[key] = std::move(written);
    }
}

} // namespace

json::Value texture_schema() {
    json::Value schema;
    schema["$schema"] = "https://json-schema.org/draft/2020-12/schema";
    schema["title"] = "SlashEngine procedural texture";
    schema["description"] = "A texture made as it loads, as Godot's NoiseTexture2D, "
                            "GradientTexture1D or GradientTexture2D; \"type\" says which.";
    schema["oneOf"] =
        json::Array{noise_texture_schema(), gradient_1d_schema(), gradient_2d_schema()};
    return schema;
}

std::vector<json::ValidationError> validate_texture(const json::Value& doc) {
    if (!doc.is_object()) {
        return {{"", "expected an object"}};
    }
    // Each type is checked against its own schema, so errors name its fields.
    const json::Value* type = doc.find("type");
    if (type == nullptr || !type->is_string()) {
        return {
            {"/type", R"(expected "NoiseTexture2D", "GradientTexture1D" or "GradientTexture2D")"}};
    }
    const std::string& name = type->as_string();
    if (name == kNoiseType) {
        return json::validate(doc, noise_texture_schema());
    }
    if (name == kGradient1DType) {
        return json::validate(doc, gradient_1d_schema());
    }
    if (name == kGradient2DType) {
        return json::validate(doc, gradient_2d_schema());
    }
    std::string message = "unknown texture type \"" + name +
                          R"(": expected "NoiseTexture2D", "GradientTexture1D" or )"
                          R"("GradientTexture2D")";
    return {{"/type", std::move(message)}};
}

Result<ProceduralTexture> parse_texture(const json::Value& doc) {
    if (const auto errors = validate_texture(doc); !errors.empty()) {
        return detail::first_error(errors);
    }
    const std::string& type = doc.find("type")->as_string();
    const auto gradient_at = [&doc](std::string_view key) -> Gradient {
        const json::Value* value = doc.find(key);
        return value != nullptr ? parse_gradient(*value).value() : Gradient{};
    };
    if (type == kNoiseType) {
        NoiseTexture2D t;
        detail::read(doc, "width", t.width);
        detail::read(doc, "height", t.height);
        detail::read(doc, "invert", t.invert);
        detail::read(doc, "in_3d_space", t.in_3d_space);
        detail::read(doc, "seamless", t.seamless);
        detail::read(doc, "seamless_blend_skirt", t.seamless_blend_skirt);
        detail::read(doc, "as_normal_map", t.as_normal_map);
        detail::read(doc, "bump_strength", t.bump_strength);
        detail::read(doc, "normalize", t.normalize);
        if (doc.find("color_ramp") != nullptr) {
            t.color_ramp = gradient_at("color_ramp");
        }
        if (const json::Value* noise = doc.find("noise"); noise != nullptr) {
            t.noise = parse_noise(*noise).value();
        }
        return ProceduralTexture(std::move(t));
    }
    if (type == kGradient1DType) {
        GradientTexture1D t;
        detail::read(doc, "width", t.width);
        t.gradient = gradient_at("gradient");
        return ProceduralTexture(std::move(t));
    }
    GradientTexture2D t;
    detail::read(doc, "width", t.width);
    detail::read(doc, "height", t.height);
    detail::read(doc, "fill", kFills, t.fill);
    t.fill_from = read_point(doc, "fill_from", t.fill_from);
    t.fill_to = read_point(doc, "fill_to", t.fill_to);
    detail::read(doc, "repeat", kRepeats, t.repeat);
    t.gradient = gradient_at("gradient");
    return ProceduralTexture(std::move(t));
}

json::Value texture_to_json(const ProceduralTexture& texture) {
    json::Value out;
    out["format"] = 1;
    using detail::write;
    if (const auto* t = std::get_if<NoiseTexture2D>(&texture); t != nullptr) {
        const NoiseTexture2D d;
        out["type"] = kNoiseType;
        write(out, "width", t->width, d.width);
        write(out, "height", t->height, d.height);
        write(out, "invert", t->invert, d.invert);
        write(out, "in_3d_space", t->in_3d_space, d.in_3d_space);
        write(out, "seamless", t->seamless, d.seamless);
        write(out, "seamless_blend_skirt", t->seamless_blend_skirt, d.seamless_blend_skirt);
        write(out, "as_normal_map", t->as_normal_map, d.as_normal_map);
        write(out, "bump_strength", t->bump_strength, d.bump_strength);
        write(out, "normalize", t->normalize, d.normalize);
        if (t->color_ramp) {
            out["color_ramp"] = gradient_to_json(*t->color_ramp);
        }
        json::Value noise = noise_to_json(t->noise);
        if (noise.size() > 0) {
            out["noise"] = std::move(noise);
        }
    } else if (const auto* g1 = std::get_if<GradientTexture1D>(&texture); g1 != nullptr) {
        out["type"] = kGradient1DType;
        write(out, "width", g1->width, GradientTexture1D{}.width);
        write_gradient(out, "gradient", g1->gradient);
    } else {
        const auto& g2 = std::get<GradientTexture2D>(texture);
        const GradientTexture2D d;
        out["type"] = kGradient2DType;
        write(out, "width", g2.width, d.width);
        write(out, "height", g2.height, d.height);
        write(out, "fill", kFills, g2.fill, d.fill);
        write_point(out, "fill_from", g2.fill_from, d.fill_from);
        write_point(out, "fill_to", g2.fill_to, d.fill_to);
        write(out, "repeat", kRepeats, g2.repeat, d.repeat);
        write_gradient(out, "gradient", g2.gradient);
    }
    return out;
}

std::vector<std::uint8_t> noise_image(const Noise& noise, int width, int height, bool invert,
                                      bool in_3d_space, bool normalize, bool seamless,
                                      float blend_skirt) {
    if (width <= 0 || height <= 0) {
        return {};
    }
    if (!seamless) {
        return gray_image(noise, width, height, invert, in_3d_space, normalize);
    }
    // Noise::_get_seamless_image: a larger image, by the skirt, made seamless.
    const int skirt_width =
        static_cast<int>(std::max(1.0F, static_cast<float>(width) * blend_skirt));
    const int skirt_height =
        static_cast<int>(std::max(1.0F, static_cast<float>(height) * blend_skirt));
    return seamless_image(gray_image(noise, width + skirt_width, height + skirt_height, invert,
                                     in_3d_space, normalize),
                          width, height, blend_skirt);
}

void bump_to_normal_map(render::Image& image, float bump_scale) {
    const auto width = static_cast<int>(image.width);
    const auto height = static_cast<int>(image.height);
    if (width == 0 || height == 0) {
        return;
    }
    // Image::convert(FORMAT_RF): the red channel as a float.
    std::vector<float> heights(static_cast<std::size_t>(width) * static_cast<std::size_t>(height));
    for (std::size_t i = 0; i < heights.size(); ++i) {
        heights[i] = static_cast<float>(image.pixels[i * 4] / 255.0);
    }
    const auto at = [&](int x, int y) {
        return heights[static_cast<std::size_t>(y) * static_cast<std::size_t>(width) +
                       static_cast<std::size_t>(x)];
    };
    for (int ty = 0; ty < height; ty++) {
        int py = ty + 1;
        if (py >= height) {
            py -= height;
        }
        for (int tx = 0; tx < width; tx++) {
            int px = tx + 1;
            if (px >= width) {
                px -= width;
            }
            const float here = at(tx, ty);
            const float to_right = at(px, ty);
            const float above = at(tx, py);
            // across (1, 0, dx) crossed with up (0, 1, dy), as Vector3::cross.
            const Vec3 up{0.0F, 1.0F, (here - above) * bump_scale};
            const Vec3 across{1.0F, 0.0F, (to_right - here) * bump_scale};
            Vec3 normal{across.y * up.z - across.z * up.y, across.z * up.x - across.x * up.z,
                        across.x * up.y - across.y * up.x};
            // Vector3::normalize.
            const float length_squared =
                normal.x * normal.x + normal.y * normal.y + normal.z * normal.z;
            if (length_squared == 0.0F) {
                normal = {};
            } else {
                const float length = std::sqrt(length_squared);
                normal = {normal.x / length, normal.y / length, normal.z / length};
            }
            std::uint8_t* out = image.pixels.data() +
                                (static_cast<std::size_t>(ty) * static_cast<std::size_t>(width) +
                                 static_cast<std::size_t>(tx)) *
                                    4;
            out[0] = static_cast<std::uint8_t>(127.5 + normal.x * 127.5);
            out[1] = static_cast<std::uint8_t>(127.5 + normal.y * 127.5);
            out[2] = static_cast<std::uint8_t>(127.5 + normal.z * 127.5);
            out[3] = 255;
        }
    }
}

bool is_texture_path(std::string_view path) {
    return render::is_image_path(path) || path.ends_with(kTextureSuffix) ||
           asset::is_made_path(path);
}

Result<render::Image> decode_texture(std::string_view path, std::string_view bytes) {
    if (!path.ends_with(kTextureSuffix)) {
        return render::decode_image(bytes);
    }
    auto doc = json::parse(bytes);
    if (!doc) {
        return doc.error();
    }
    auto texture = parse_texture(doc.value());
    if (!texture) {
        return texture.error();
    }
    return make_image(texture.value());
}

render::Image make_image(const ProceduralTexture& texture) {
    if (const auto* noise = std::get_if<NoiseTexture2D>(&texture); noise != nullptr) {
        return noise_texture_image(*noise);
    }
    if (const auto* g1 = std::get_if<GradientTexture1D>(&texture); g1 != nullptr) {
        return gradient_1d_image(*g1);
    }
    return gradient_2d_image(std::get<GradientTexture2D>(texture));
}

Result<std::string> make_texture(world::World& world, std::string_view name, render::Image image) {
    asset::Assets* assets = asset::assets_of(world.ecs());
    if (assets == nullptr) {
        return make_error(ErrorCode::NotFound,
                          "the world has no assets to make a texture in (canvas::install "
                          "attaches them)");
    }
    const std::size_t expected =
        static_cast<std::size_t>(image.width) * static_cast<std::size_t>(image.height) * 4;
    if (image.width == 0 || image.height == 0 || image.pixels.size() != expected) {
        return make_error(
            ErrorCode::InvalidArgument,
            "a texture's pixels are width x height x 4 bytes (RGBA8): " + std::to_string(expected) +
                " for " + std::to_string(image.width) + " x " + std::to_string(image.height) +
                ", not " + std::to_string(image.pixels.size()));
    }
    image.blocks.clear();
    std::string path = std::string(asset::kMadePrefix) + std::string(name);
    const std::size_t bytes = sizeof(render::Image) + image.pixels.capacity();
    if (auto put = assets->put<render::Image>(path, std::move(image), bytes); !put) {
        return put.error();
    }
    return path;
}

} // namespace slash::procedural
