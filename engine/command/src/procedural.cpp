#include "procedural.hpp"

#include "slash/command/session.hpp"
#include "slash/platform/file.hpp"
#include "slash/procedural/texture.hpp"
#include "slash/render/image.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace slash::command {
namespace {

constexpr std::int64_t kMaxPixels = 256;
constexpr std::string_view kPng = ".png";

json::Value property(std::string_view type, std::string_view description) {
    json::Value schema;
    schema["type"] = type;
    schema["description"] = description;
    return schema;
}

json::Value params_schema(json::Object properties) {
    json::Value schema;
    schema["type"] = "object";
    schema["additionalProperties"] = false;
    schema["properties"] = std::move(properties);
    return schema;
}

Reply param_error(int error_code, std::string_view path, const std::string& message) {
    json::Value data;
    data["path"] = path;
    return Reply::failure(error_code, std::string(path) + ": " + message, std::move(data));
}

std::string quoted(std::string_view name) {
    return "\"" + std::string(name) + "\"";
}

// A byte average rounded to two decimals, with no ".0" on whole ones.
json::Value short_mean(double value) {
    const double out = std::round(value * 100.0) / 100.0;
    if (out == std::floor(out)) {
        return static_cast<std::int64_t>(out);
    }
    return out;
}

// Each channel's [min, mean, max] over the image, 0 to 255.
json::Value channel_stats(const render::Image& image) {
    constexpr std::array<std::string_view, 4> kChannels{"r", "g", "b", "a"};
    const std::size_t count = image.pixels.size() / 4;
    json::Value out;
    for (std::size_t c = 0; c < 4; ++c) {
        int low = 255;
        int high = 0;
        std::uint64_t sum = 0;
        for (std::size_t i = 0; i < count; ++i) {
            const int value = image.pixels[i * 4 + c];
            low = std::min(low, value);
            high = std::max(high, value);
            sum += static_cast<std::uint64_t>(value);
        }
        out[kChannels.at(c)] =
            count == 0
                ? json::Array{0, 0, 0}
                : json::Array{
                      low, short_mean(static_cast<double>(sum) / static_cast<double>(count)), high};
    }
    return out;
}

// How far apart, on average per channel, neighboring pixels are inside the image and across its
// edges when it repeats: a texture tiles without seams when "x" and "y" are near "inside".
json::Value wrap_differences(const render::Image& image) {
    const auto width = static_cast<std::size_t>(image.width);
    const auto height = static_cast<std::size_t>(image.height);
    const auto difference = [&](std::size_t a, std::size_t b) {
        int total = 0;
        for (std::size_t c = 0; c < 4; ++c) {
            total += std::abs(int{image.pixels[a * 4 + c]} - int{image.pixels[b * 4 + c]});
        }
        return static_cast<double>(total) / 4.0;
    };
    double inside = 0.0;
    std::size_t inside_count = 0;
    for (std::size_t y = 0; y < height; ++y) {
        for (std::size_t x = 0; x + 1 < width; ++x) {
            inside += difference(y * width + x, y * width + x + 1);
            ++inside_count;
        }
    }
    double across_x = 0.0;
    for (std::size_t y = 0; y < height; ++y) {
        across_x += difference(y * width + width - 1, y * width);
    }
    double across_y = 0.0;
    for (std::size_t x = 0; x < width; ++x) {
        across_y += difference((height - 1) * width + x, x);
    }
    json::Value out;
    out["inside"] = short_mean(inside_count > 0 ? inside / static_cast<double>(inside_count) : 0);
    out["x"] = short_mean(height > 0 ? across_x / static_cast<double>(height) : 0.0);
    out["y"] = short_mean(width > 0 ? across_y / static_cast<double>(width) : 0.0);
    return out;
}

Reply texture_generate(Session& session, const json::Value& params) {
    const json::Value* path = params.find("path");
    const json::Value* inline_texture = params.find("texture");
    if ((path == nullptr) == (inline_texture == nullptr)) {
        return param_error(code::kInvalidParams, "", R"(give "path" or "texture", not both)");
    }
    const std::string out_path =
        params.find("out") != nullptr ? params.find("out")->as_string() : std::string();
    if (!out_path.empty()) {
        if (!session.allowed(Capability::Filesystem)) {
            json::Value data;
            data["capability"] = "filesystem";
            data["path"] = "/out";
            return Reply::failure(code::kPermissionDenied,
                                  "/out: writing a file needs the \"filesystem\" capability, "
                                  "which this session does not grant",
                                  std::move(data));
        }
        if (!out_path.ends_with(kPng) || out_path.size() == kPng.size()) {
            return param_error(code::kInvalidArgument, "/out",
                               R"(textures are written as .png files, e.g. "build/noise.png")");
        }
    }

    render::Image made;
    asset::Handle<render::Image> loaded;
    const render::Image* image = nullptr;
    if (path != nullptr) {
        const std::string& file = path->as_string();
        if (!procedural::is_texture_path(file)) {
            return param_error(
                code::kInvalidArgument, "/path",
                "expected a .texture.json, .png, .bmp, .jpg, .jpeg or .ktx2 file, or a made "
                "texture (\"made:<name>\")");
        }
        auto handle = session.assets().load<render::Image>(file);
        if (!handle) {
            return param_error(handle.error().code == ErrorCode::NotFound ? code::kNotFound
                                                                          : code::kInvalidArgument,
                               "/path", handle.error().message);
        }
        loaded = std::move(handle).value();
        image = loaded.get();
    } else {
        std::vector<json::ValidationError> errors = procedural::validate_texture(*inline_texture);
        if (!errors.empty()) {
            for (json::ValidationError& error : errors) {
                error.path = "/texture" + error.path;
            }
            return Reply::invalid_params(errors);
        }
        made = procedural::make_image(procedural::parse_texture(*inline_texture).value());
        image = &made;
    }

    json::Value result;
    result["width"] = image->width;
    result["height"] = image->height;
    result["channels"] = channel_stats(*image);
    result["wrap"] = wrap_differences(*image);
    if (const json::Value* pixels = params.find("pixels"); pixels != nullptr) {
        json::Array colors;
        const json::Array& points = pixels->as_array();
        for (std::size_t i = 0; i < points.size(); ++i) {
            const std::int64_t x = points[i].as_array()[0].as_int();
            const std::int64_t y = points[i].as_array()[1].as_int();
            if (x >= image->width || y >= image->height) {
                return param_error(code::kInvalidArgument, "/pixels/" + std::to_string(i),
                                   "outside the " + std::to_string(image->width) + " by " +
                                       std::to_string(image->height) + " texture");
            }
            const std::size_t at =
                (static_cast<std::size_t>(y) * image->width + static_cast<std::size_t>(x)) * 4;
            colors.emplace_back(json::Array{image->pixels[at], image->pixels[at + 1],
                                            image->pixels[at + 2], image->pixels[at + 3]});
        }
        result["pixels"] = std::move(colors);
    }
    if (!out_path.empty()) {
        auto file = session.resolve_path(out_path);
        if (!file) {
            return param_error(code::kInvalidArgument, "/out", file.error().message);
        }
        if (!session.dry_running()) {
            render::Image plain = *image;
            plain.blocks.clear();
            auto png = render::encode_png(plain);
            if (!png) {
                return Reply::failure(png.error());
            }
            if (auto written = session.write_file(file.value(), png.value()); !written) {
                return Reply::failure(written.error());
            }
        }
        result["out"] = out_path;
    }
    return Reply::success(std::move(result));
}

} // namespace

json::Value noise_source_property(std::string_view description) {
    json::Value schema;
    schema["type"] = json::Array{"string", "object"};
    schema["description"] = description;
    return schema;
}

std::optional<Reply> read_noise_source(Session& session, const json::Value& value,
                                       const std::string& at, NoiseSource& out) {
    if (value.is_object()) {
        const std::vector<json::ValidationError> errors = procedural::validate_noise(value, at);
        if (!errors.empty()) {
            return Reply::invalid_params(errors);
        }
        out = NoiseSource{.settings = procedural::parse_noise(value).value()};
        return std::nullopt;
    }
    const std::string& file = value.as_string();
    if (!file.ends_with(procedural::kTextureSuffix)) {
        return param_error(code::kInvalidArgument, at,
                           "expected a .texture.json file of a NoiseTexture2D, or noise settings "
                           "as an object");
    }
    // Read as text: the asset of a .texture.json path is its image.
    auto resolved = session.resolve_path(file);
    auto text =
        resolved ? platform::read_file(resolved.value()) : Result<std::string>(resolved.error());
    if (!text) {
        if (text.error().code == ErrorCode::NotFound) {
            return param_error(code::kNotFound, at, "the project has no file " + quoted(file));
        }
        return param_error(code::kInvalidArgument, at, text.error().message);
    }
    auto doc = json::parse(text.value());
    if (!doc) {
        return param_error(code::kInvalidArgument, at,
                           quoted(file) + " is not valid JSON: " + doc.error().message);
    }
    const std::vector<json::ValidationError> errors = procedural::validate_texture(doc.value());
    if (!errors.empty()) {
        const json::ValidationError& first = errors.front();
        return param_error(code::kInvalidArgument, at,
                           quoted(file) + (first.path.empty() ? "" : " at " + first.path) + ": " +
                               first.message);
    }
    const procedural::ProceduralTexture texture = procedural::parse_texture(doc.value()).value();
    const auto* noise = std::get_if<procedural::NoiseTexture2D>(&texture);
    if (noise == nullptr) {
        return param_error(code::kInvalidArgument, at,
                           quoted(file) + " is a gradient texture; name a NoiseTexture2D");
    }
    out = NoiseSource{
        .settings = noise->noise, .normalize = noise->normalize, .invert = noise->invert};
    return std::nullopt;
}

std::vector<float> sample_noise_source(const NoiseSource& source, int x, int y, int width,
                                       int height) {
    const procedural::Noise noise(source.settings);
    std::vector<float> values;
    values.reserve(static_cast<std::size_t>(width) * static_cast<std::size_t>(height));
    float low = std::numeric_limits<float>::max();
    float high = std::numeric_limits<float>::lowest();
    for (int row = 0; row < height; ++row) {
        for (int column = 0; column < width; ++column) {
            const float value =
                noise.get_noise_2d(static_cast<float>(x + column), static_cast<float>(y + row));
            low = std::min(low, value);
            high = std::max(high, value);
            values.push_back(value);
        }
    }
    // As Godot's Noise::_get_image, in floats rather than bytes.
    for (float& value : values) {
        if (source.normalize) {
            value = high == low ? 0.0F : std::clamp((value - low) / (high - low), 0.0F, 1.0F);
        } else {
            value = std::clamp((value * 0.5F) + 0.5F, 0.0F, 1.0F);
        }
        if (source.invert) {
            value = 1.0F - value;
        }
    }
    return values;
}

void register_procedural_commands(Registry& registry) {
    using enum Capability;
    json::Value pixel = property("array", "[x, y] from the top left.");
    pixel["items"]["type"] = "integer";
    pixel["items"]["minimum"] = 0;
    pixel["minItems"] = 2;
    pixel["maxItems"] = 2;
    json::Value pixels = property("array", "Pixels to return as [r, g, b, a] bytes.");
    pixels["items"] = std::move(pixel);
    pixels["maxItems"] = kMaxPixels;
    json::Value texture =
        property("object", "A .texture.json document to make without a file, to try settings.");
    registry.add({
        .name = "texture.generate",
        .description =
            "Makes a texture, a .texture.json file (\"path\", any texture file works) or one "
            "given inline (\"texture\"), and measures it so you can tune it without looking: "
            "size, each channel's [min, mean, max] (0-255) and \"wrap\", the mean difference of "
            "neighboring pixels inside and across the edges (x, y near inside: it tiles). "
            "\"pixels\" returns those pixels; \"out\" writes a .png (needs filesystem).",
        .capabilities = {Read},
        .effect = Effect::Read,
        .params = params_schema({
            {"path", property("string", "A texture file.")},
            {"texture", std::move(texture)},
            {"pixels", std::move(pixels)},
            {"out", property("string", "A .png file to write the texture to.")},
        }),
        .handler = texture_generate,
    });
}

} // namespace slash::command
