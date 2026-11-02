#include "slash/asset/assets.hpp"
#include "slash/canvas/canvas.hpp"
#include "slash/command/builtin.hpp"
#include "slash/command/command.hpp"
#include "slash/command/session.hpp"
#include "slash/core/json.hpp"
#include "slash/core/log.hpp"
#include "slash/core/math.hpp"
#include "slash/platform/file.hpp"
#include "slash/procedural/gradient.hpp"
#include "slash/procedural/noise.hpp"
#include "slash/procedural/scatter.hpp"
#include "slash/procedural/texture.hpp"
#include "slash/render/image.hpp"
#include "slash/scene3d/surface_tool.hpp"
#include "slash/ui/ui.hpp"
#include "slash/world/component_registry.hpp"
#include "slash/world/components.hpp"
#include "slash/world/tree3d.hpp"

#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <tuple>
#include <utility>
#include <variant>
#include <vector>

namespace asset = slash::asset;
namespace canvas = slash::canvas;
namespace command = slash::command;
namespace json = slash::json;
namespace procedural = slash::procedural;
namespace render = slash::render;
namespace world = slash::world;

using slash::Vec2;
using slash::Vec3;
using slash::Vec4;

namespace {

json::Value parse(std::string_view text) {
    auto parsed = json::parse(text);
    REQUIRE_MESSAGE(parsed.ok(), text);
    return std::move(parsed).value();
}

procedural::ProceduralTexture texture_of(std::string_view text) {
    auto parsed = procedural::parse_texture(parse(text));
    REQUIRE_MESSAGE(parsed.ok(), parsed.error().message);
    return std::move(parsed).value();
}

render::Image image_of(std::string_view text) {
    return procedural::make_image(texture_of(text));
}

std::vector<int> pixel(const render::Image& image, std::uint32_t x, std::uint32_t y) {
    const std::size_t at = (static_cast<std::size_t>(y) * image.width + x) * 4;
    return {image.pixels[at], image.pixels[at + 1], image.pixels[at + 2], image.pixels[at + 3]};
}

// The mean difference of a channel between neighbors inside the image, and across its left and
// right edges when it repeats.
std::pair<double, double> seam_difference(const render::Image& image) {
    double inside = 0.0;
    double across = 0.0;
    for (std::uint32_t y = 0; y < image.height; ++y) {
        for (std::uint32_t x = 0; x + 1 < image.width; ++x) {
            inside += std::abs(pixel(image, x, y)[0] - pixel(image, x + 1, y)[0]);
        }
        across += std::abs(pixel(image, image.width - 1, y)[0] - pixel(image, 0, y)[0]);
    }
    return {inside / static_cast<double>(image.height * (image.width - 1)),
            across / static_cast<double>(image.height)};
}

struct MemoryFiles {
    std::map<std::string, std::string, std::less<>> files;

    asset::FileSource source() {
        return {
            .read = [this](std::string_view path) -> slash::Result<std::string> {
                const auto it = files.find(path);
                if (it == files.end()) {
                    return slash::make_error(slash::ErrorCode::NotFound, "no such file");
                }
                return it->second;
            },
            .stamp = [this](std::string_view path) -> std::optional<asset::FileStamp> {
                const auto it = files.find(path);
                if (it == files.end()) {
                    return std::nullopt;
                }
                return asset::FileStamp{.modified = 1, .size = it->second.size()};
            },
        };
    }
};

} // namespace

TEST_CASE("noise samples as Godot's FastNoiseLite, the same on every machine") {
    const procedural::Noise noise;
    // Godot's defaults: simplex smooth, fractal Brownian motion of five octaves, frequency 0.01.
    CHECK(noise.settings().noise_type == procedural::NoiseType::SimplexSmooth);
    CHECK(noise.settings().fractal_octaves == 5);

    // The same bits on every machine, so these are exact.
    CHECK(noise.get_noise_2d(10.0F, 20.0F) == 0.304509848F);
    CHECK(noise.get_noise_3d(10.0F, 20.0F, 30.0F) == 0.136608183F);
    CHECK(noise.get_noise_1d(42.0F) == noise.get_noise_2d(42.0F, 0.0F));

    // Every type stays near -1 to 1, and seeds change it.
    for (const procedural::NoiseType type :
         {procedural::NoiseType::Simplex, procedural::NoiseType::SimplexSmooth,
          procedural::NoiseType::Cellular, procedural::NoiseType::Perlin,
          procedural::NoiseType::ValueCubic, procedural::NoiseType::Value}) {
        procedural::NoiseSettings settings;
        settings.noise_type = type;
        settings.cellular_return_type = procedural::CellularReturn::CellValue;
        const procedural::Noise a(settings);
        settings.seed = 9;
        const procedural::Noise b(settings);
        bool differs = false;
        for (int i = 0; i < 64; ++i) {
            const float x = static_cast<float>(i) * 13.7F;
            const float value = a.get_noise_2d(x, x * 0.5F);
            CHECK(std::abs(value) <= 1.01F);
            differs = differs || value != b.get_noise_2d(x, x * 0.5F);
        }
        CHECK(differs);
    }

    // The offset moves the sampled point; copies and other threads sample the same values.
    procedural::NoiseSettings moved;
    moved.offset = {100.0F, 50.0F, 0.0F};
    CHECK(procedural::Noise(moved).get_noise_2d(0.0F, 0.0F) == noise.get_noise_2d(100.0F, 50.0F));
    const procedural::Noise copy = noise;
    CHECK(copy.get_noise_2d(10.0F, 20.0F) == noise.get_noise_2d(10.0F, 20.0F));
    float on_thread = 0.0F;
    std::thread thread([&] { on_thread = noise.get_noise_2d(10.0F, 20.0F); });
    thread.join();
    CHECK(on_thread == noise.get_noise_2d(10.0F, 20.0F));

    // Domain warping moves points before sampling.
    procedural::NoiseSettings warped;
    warped.domain_warp_enabled = true;
    CHECK(procedural::Noise(warped).get_noise_2d(10.0F, 20.0F) != noise.get_noise_2d(10.0F, 20.0F));

    // get_values samples whole coordinates, rows from the top.
    const std::vector<float> values = noise.get_values(4, 3);
    REQUIRE(values.size() == 12);
    CHECK(values[1 * 4 + 2] == noise.get_noise_2d(2.0F, 1.0F));
    CHECK(noise.get_values(2, 2, true, 5)[3] == noise.get_noise_3d(1.0F, 1.0F, 5.0F));
}

TEST_CASE("noise settings read and write Godot's property names") {
    const json::Value doc = parse(R"({"noise_type": "cellular", "seed": 4, "frequency": 0.05,
        "offset": [1, 2, 3], "fractal_type": "ping_pong", "cellular_return_type": "distance2_div",
        "domain_warp_enabled": true, "domain_warp_type": "basic_grid"})");
    CHECK(procedural::validate_noise(doc).empty());
    const procedural::NoiseSettings settings = procedural::parse_noise(doc).value();
    CHECK(settings.noise_type == procedural::NoiseType::Cellular);
    CHECK(settings.seed == 4);
    CHECK(settings.frequency == 0.05F);
    CHECK(settings.offset == slash::Vec3{1.0F, 2.0F, 3.0F});
    CHECK(settings.fractal_type == procedural::FractalType::PingPong);
    CHECK(settings.cellular_return_type == procedural::CellularReturn::Distance2Div);
    CHECK(settings.domain_warp_type == procedural::DomainWarpType::BasicGrid);
    // Written back, defaults are left out.
    CHECK(procedural::noise_to_json(settings) == doc);
    CHECK(procedural::noise_to_json({}) == json::Value(json::Object{}));

    const auto errors = procedural::validate_noise(
        parse(R"({"noise_type": "simplx", "fractal_octaves": 0, "colour": 1})"), "/noise");
    REQUIRE(errors.size() == 3);
    bool named = false;
    for (const json::ValidationError& error : errors) {
        CHECK(error.path.starts_with("/noise"));
        named = named || error.message.find("simplex") != std::string::npos;
    }
    CHECK(named);
}

TEST_CASE("gradients blend as Godot's") {
    procedural::Gradient gradient;
    // Black to white by default; outside the points, the end colors.
    CHECK(gradient.color_at(0.5F) == Vec4{0.5F, 0.5F, 0.5F, 1.0F});
    CHECK(gradient.color_at(-1.0F) == Vec4{0.0F, 0.0F, 0.0F, 1.0F});
    CHECK(gradient.color_at(2.0F) == Vec4{1.0F, 1.0F, 1.0F, 1.0F});

    gradient.interpolation_mode = procedural::GradientInterpolation::Constant;
    CHECK(gradient.color_at(0.99F) == Vec4{0.0F, 0.0F, 0.0F, 1.0F});
    CHECK(gradient.color_at(1.0F) == Vec4{1.0F, 1.0F, 1.0F, 1.0F});

    // Linear sRGB blends brighter in the middle than sRGB does, Oklab darker.
    gradient.interpolation_mode = procedural::GradientInterpolation::Linear;
    gradient.interpolation_color_space = procedural::GradientColorSpace::LinearSrgb;
    const float linear_mid = gradient.color_at(0.5F).x;
    CHECK(linear_mid == doctest::Approx(0.7353569F).epsilon(1.0e-5));
    gradient.interpolation_color_space = procedural::GradientColorSpace::Oklab;
    CHECK(gradient.color_at(0.5F).x == doctest::Approx(0.388573F).epsilon(1.0e-4));
    CHECK(gradient.color_at(0.0F).x == 0.0F);

    // Cubic passes through each point and eases between them, by Godot's cubic_interpolate.
    procedural::Gradient three = procedural::parse_gradient(parse(R"({
        "points": [{"offset": 1, "color": [1, 0, 0]}, {"offset": 0, "color": [0, 0, 0]},
                   {"offset": 0.5, "color": [0.2, 0, 0, 0.5]}],
        "interpolation_mode": "cubic"})"))
                                     .value();
    // Points are sorted by offset.
    CHECK(three.points[1].offset == 0.5F);
    CHECK(three.color_at(0.5F) == Vec4{0.2F, 0.0F, 0.0F, 0.5F});
    CHECK(three.color_at(0.25F).x == doctest::Approx(0.05F));

    // Color conversions go there and back.
    const Vec4 color{0.25F, 0.5F, 0.75F, 0.5F};
    const Vec4 back = procedural::linear_to_srgb(procedural::oklab_to_linear(
        procedural::linear_to_oklab(procedural::srgb_to_linear(color))));
    CHECK(back.x == doctest::Approx(color.x).epsilon(1.0e-4));
    CHECK(back.y == doctest::Approx(color.y).epsilon(1.0e-4));
    CHECK(back.z == doctest::Approx(color.z).epsilon(1.0e-4));
    CHECK(back.w == color.w);

    CHECK(procedural::gradient_to_json(three) ==
          parse(R"({"points": [{"offset": 0, "color": [0, 0, 0, 1]},
                               {"offset": 0.5, "color": [0.2, 0, 0, 0.5]},
                               {"offset": 1, "color": [1, 0, 0, 1]}],
                    "interpolation_mode": "cubic"})"));
}

TEST_CASE("gradient textures fill as Godot's GradientTexture1D and GradientTexture2D") {
    const render::Image row = image_of(R"({"format": 1, "type": "GradientTexture1D", "width": 3})");
    CHECK(row.width == 3);
    CHECK(row.height == 1);
    CHECK(pixel(row, 0, 0) == std::vector<int>{0, 0, 0, 255});
    CHECK(pixel(row, 1, 0) == std::vector<int>{128, 128, 128, 255});
    CHECK(pixel(row, 2, 0) == std::vector<int>{255, 255, 255, 255});
    // One pixel wide, the first color, where Godot divides by zero.
    CHECK(pixel(image_of(R"({"format": 1, "type": "GradientTexture1D", "width": 1})"), 0, 0) ==
          std::vector<int>{0, 0, 0, 255});

    // Linear along fill_from to fill_to, held past it.
    const render::Image linear = image_of(R"({"format": 1, "type": "GradientTexture2D",
        "width": 5, "height": 5, "fill_to": [0.5, 0]})");
    CHECK(pixel(linear, 0, 3) == std::vector<int>{0, 0, 0, 255});
    CHECK(pixel(linear, 1, 0) == std::vector<int>{128, 128, 128, 255});
    CHECK(pixel(linear, 4, 4) == std::vector<int>{255, 255, 255, 255});

    // Radial from the middle, the same on every side.
    const render::Image radial = image_of(R"({"format": 1, "type": "GradientTexture2D",
        "width": 5, "height": 5, "fill": "radial", "fill_from": [0.5, 0.5], "fill_to": [1, 0.5]})");
    CHECK(pixel(radial, 2, 2) == std::vector<int>{0, 0, 0, 255});
    CHECK(pixel(radial, 0, 2) == pixel(radial, 4, 2));
    CHECK(pixel(radial, 2, 0) == pixel(radial, 2, 4));
    CHECK(pixel(radial, 4, 2) == std::vector<int>{255, 255, 255, 255});
    // Square fills reach the corners at the same offset as the sides.
    const render::Image square = image_of(R"({"format": 1, "type": "GradientTexture2D",
        "width": 5, "height": 5, "fill": "square", "fill_from": [0.5, 0.5], "fill_to": [1, 0.5]})");
    CHECK(pixel(square, 0, 0) == pixel(square, 4, 2));

    // Repeat starts again past fill_to; mirror comes back.
    const render::Image repeat = image_of(R"({"format": 1, "type": "GradientTexture2D",
        "width": 9, "height": 1, "fill_to": [0.5, 0], "repeat": "repeat"})");
    CHECK(pixel(repeat, 2, 0) == pixel(repeat, 6, 0));
    const render::Image mirror = image_of(R"({"format": 1, "type": "GradientTexture2D",
        "width": 9, "height": 1, "fill_to": [0.5, 0], "repeat": "mirror"})");
    CHECK(pixel(mirror, 2, 0) == pixel(mirror, 6, 0));
    CHECK(pixel(mirror, 3, 0) == pixel(mirror, 5, 0));
    CHECK(pixel(mirror, 8, 0) == std::vector<int>{0, 0, 0, 255});

    // A gradient of one color fills with it.
    CHECK(pixel(image_of(R"({"format": 1, "type": "GradientTexture2D", "width": 2, "height": 2,
        "gradient": {"points": [{"offset": 0.3, "color": [1, 0.5, 0]}]}})"),
                1, 1) == std::vector<int>{255, 127, 0, 255});
}

TEST_CASE("noise textures make Godot's NoiseTexture2D pixels") {
    const render::Image plain =
        image_of(R"({"format": 1, "type": "NoiseTexture2D", "width": 64, "height": 32})");
    CHECK(plain.width == 64);
    CHECK(plain.height == 32);
    // Gray and opaque, stretched over the full range.
    int low = 255;
    int high = 0;
    for (std::uint32_t y = 0; y < plain.height; ++y) {
        for (std::uint32_t x = 0; x < plain.width; ++x) {
            const std::vector<int> p = pixel(plain, x, y);
            CHECK(p[0] == p[1]);
            CHECK(p[1] == p[2]);
            CHECK(p[3] == 255);
            low = std::min(low, p[0]);
            high = std::max(high, p[0]);
        }
    }
    CHECK(low == 0);
    CHECK(high == 255);
    CHECK(pixel(plain, 10, 20) == (std::vector<int>{192, 192, 192, 255}));

    // Inverted, each byte turns over.
    const render::Image inverted = image_of(
        R"({"format": 1, "type": "NoiseTexture2D", "width": 64, "height": 32, "invert": true})");
    CHECK(pixel(inverted, 10, 20)[0] == 255 - pixel(plain, 10, 20)[0]);

    // Without normalizing, -1 to 1 maps onto 0 to 255.
    const render::Image raw = image_of(
        R"({"format": 1, "type": "NoiseTexture2D", "width": 8, "height": 8, "normalize": false})");
    const float value = procedural::Noise().get_noise_2d(3.0F, 5.0F);
    CHECK(pixel(raw, 3, 5)[0] == static_cast<int>(value * 127.5F + 127.5F));

    // A color ramp colors each value by its luminance.
    const render::Image ramp = image_of(R"({"format": 1, "type": "NoiseTexture2D", "width": 64,
        "height": 32, "color_ramp": {"points": [{"offset": 0, "color": [0, 0, 1]},
                                               {"offset": 1, "color": [1, 0, 0]}]}})");
    for (std::uint32_t x = 0; x < 64; ++x) {
        const std::vector<int> p = pixel(ramp, x, 7);
        CHECK(p[1] == 0);
        CHECK(std::abs(p[0] + p[2] - 255) <= 1);
    }
}

TEST_CASE("seamless noise textures tile") {
    const std::string_view settings = R"({"format": 1, "type": "NoiseTexture2D", "width": 64,
        "height": 64, "noise": {"frequency": 0.05}})";
    const auto [inside, across] = seam_difference(image_of(settings));
    // Plain noise jumps where it repeats.
    CHECK(across > inside * 2.0);

    json::Value doc = parse(settings);
    doc["seamless"] = true;
    const render::Image seamless = procedural::make_image(procedural::parse_texture(doc).value());
    const auto [seamless_inside, seamless_across] = seam_difference(seamless);
    CHECK(seamless_across < seamless_inside * 1.5);
    // Across the top and bottom too.
    double vertical = 0.0;
    for (std::uint32_t x = 0; x < 64; ++x) {
        vertical += std::abs(pixel(seamless, x, 63)[0] - pixel(seamless, x, 0)[0]);
    }
    CHECK(vertical / 64.0 < seamless_inside * 1.5);
}

TEST_CASE("noise textures make normal maps as Godot's bump_map_to_normal_map") {
    // A flat bump map points straight out.
    render::Image flat;
    flat.width = 2;
    flat.height = 2;
    flat.pixels.assign(16, 100);
    procedural::bump_to_normal_map(flat, 8.0F);
    CHECK(pixel(flat, 0, 0) == std::vector<int>{127, 127, 255, 255});

    // A ramp up to the right tilts every normal left.
    render::Image ramp;
    ramp.width = 4;
    ramp.height = 1;
    ramp.pixels = {0, 0, 0, 255, 10, 0, 0, 255, 20, 0, 0, 255, 30, 0, 0, 255};
    procedural::bump_to_normal_map(ramp, 8.0F);
    CHECK(pixel(ramp, 0, 0)[0] < 127);
    CHECK(pixel(ramp, 1, 0) == pixel(ramp, 0, 0));
    CHECK(pixel(ramp, 0, 0)[1] == 127);
    // The last pixel wraps to the first, a step down, so it tilts right.
    CHECK(pixel(ramp, 3, 0)[0] > 127);

    const render::Image normals = image_of(R"({"format": 1, "type": "NoiseTexture2D",
        "width": 16, "height": 16, "as_normal_map": true})");
    for (std::uint32_t i = 0; i < 16; ++i) {
        // Normals face out of the surface.
        CHECK(pixel(normals, i, i)[2] > 127);
    }
}

TEST_CASE(".texture.json files validate, format and load as textures") {
    // Errors name the type's own fields.
    const auto unknown = procedural::validate_texture(parse(R"({"format": 1, "type": "Noise"})"));
    REQUIRE(unknown.size() == 1);
    CHECK(unknown[0].path == "/type");
    CHECK(unknown[0].message.find("NoiseTexture2D") != std::string::npos);
    const auto bad = procedural::validate_texture(parse(R"({"format": 1,
        "type": "GradientTexture2D", "width": 0, "fill": "conic",
        "gradient": {"points": [{"offset": 2, "color": [1, 1]}]}})"));
    std::vector<std::string> paths;
    for (const json::ValidationError& error : bad) {
        paths.push_back(error.path);
    }
    std::ranges::sort(paths);
    CHECK(paths == std::vector<std::string>{"/fill", "/gradient/points/0/color",
                                            "/gradient/points/0/offset", "/width"});
    CHECK_FALSE(procedural::parse_texture(parse(R"({"type": "NoiseTexture2D"})")).ok());
    CHECK(procedural::texture_schema().find("oneOf")->as_array().size() == 3);

    // Written back, only what differs from Godot's defaults stays.
    const json::Value doc = parse(R"({"format": 1, "type": "NoiseTexture2D", "width": 128,
        "seamless": true, "noise": {"noise_type": "perlin"},
        "color_ramp": {"interpolation_mode": "constant"}})");
    CHECK(procedural::texture_to_json(procedural::parse_texture(doc).value()) == doc);
    CHECK(procedural::texture_to_json(procedural::GradientTexture2D{}) ==
          parse(R"({"format": 1, "type": "GradientTexture2D"})"));

    CHECK(procedural::is_texture_path("rock.texture.json"));
    CHECK(procedural::is_texture_path("rock.png"));
    CHECK_FALSE(procedural::is_texture_path("rock.json"));
    CHECK_FALSE(procedural::decode_texture("bad.texture.json", "{").ok());

    // Canvas loads them as textures, so anything that names a .png file can name one.
    MemoryFiles files;
    files.files["sky.texture.json"] = R"({"format": 1, "type": "GradientTexture1D", "width": 16})";
    asset::Assets assets{files.source()};
    canvas::add_asset_types(assets);
    auto loaded = assets.load<render::Image>("sky.texture.json");
    REQUIRE_MESSAGE(loaded.ok(), loaded.error().message);
    CHECK(loaded.value()->width == 16);
    CHECK(pixel(*loaded.value(), 15, 0) == std::vector<int>{255, 255, 255, 255});
    files.files["broken.texture.json"] = R"({"format": 1, "type": "GradientTexture1D", "w": 1})";
    const auto broken = assets.load<render::Image>("broken.texture.json");
    REQUIRE_FALSE(broken.ok());
    CHECK(broken.error().message.find("/w") != std::string::npos);
}

TEST_CASE("texture.generate makes and measures a texture") {
    const std::string root = "procedural_test_project";
    REQUIRE(slash::platform::create_directories(root).ok());
    REQUIRE(slash::platform::write_file(root + "/ramp.texture.json",
                                        R"({"format": 1, "type": "GradientTexture1D",
                                            "width": 4})")
                .ok());

    const auto make_session = [&](std::vector<command::Capability> capabilities) {
        auto components = std::make_unique<world::ComponentRegistry>();
        world::register_builtin_components(*components);
        auto commands = std::make_unique<command::Registry>();
        command::register_builtin_commands(*commands);
        command::SessionOptions options;
        options.project_root = root;
        options.capture_logs = false;
        options.capabilities = std::move(capabilities);
        auto session = std::make_unique<command::Session>(*components, *commands, options);
        return std::make_tuple(std::move(components), std::move(commands), std::move(session));
    };
    auto [components, commands, session] = make_session(
        {command::Capability::Read, command::Capability::Edit, command::Capability::Filesystem});
    const auto call = [&](std::string_view params) {
        return session->call("texture.generate", parse(params));
    };

    // A file's texture, measured, with pixels picked out.
    command::Reply reply = call(R"({"path": "ramp.texture.json", "pixels": [[3, 0]]})");
    REQUIRE_MESSAGE(reply.ok, reply.message);
    CHECK(reply.value == parse(R"({"width": 4, "height": 1,
        "channels": {"r": [0, 127.5, 255], "g": [0, 127.5, 255], "b": [0, 127.5, 255],
                     "a": [255, 255, 255]},
        "wrap": {"inside": 63.75, "x": 191.25, "y": 0},
        "pixels": [[255, 255, 255, 255]]})"));

    // Inline settings, to try them without a file: seamless noise wraps as smoothly as inside.
    reply = call(R"({"texture": {"format": 1, "type": "NoiseTexture2D", "width": 32,
                                 "height": 32, "seamless": true,
                                 "noise": {"frequency": 0.05}}})");
    REQUIRE_MESSAGE(reply.ok, reply.message);
    const json::Value& wrap = *reply.value.find("wrap");
    CHECK(wrap.find("x")->as_double() < wrap.find("inside")->as_double() * 1.5);
    CHECK(wrap.find("y")->as_double() < wrap.find("inside")->as_double() * 1.5);

    // Written as a PNG for the asset library or a person to look at.
    reply = call(R"({"path": "ramp.texture.json", "out": "out/ramp.png"})");
    REQUIRE_MESSAGE(reply.ok, reply.message);
    CHECK(*reply.value.find("out") == parse(R"("out/ramp.png")"));
    const auto png = slash::platform::read_file(root + "/out/ramp.png");
    REQUIRE(png.ok());
    const auto decoded = render::decode_image(png.value());
    REQUIRE(decoded.ok());
    CHECK(decoded.value().width == 4);

    // Errors point at the bad value.
    reply = call(R"({"texture": {"format": 1, "type": "GradientTexture1D", "width": -1}})");
    CHECK_FALSE(reply.ok);
    CHECK(reply.message.find("/texture/width") != std::string::npos);
    CHECK_FALSE(call(R"({})").ok);
    CHECK_FALSE(call(R"({"path": "ramp.texture.json", "texture": {}})").ok);
    CHECK_FALSE(call(R"({"path": "ramp.json"})").ok);
    CHECK(call(R"({"path": "missing.texture.json"})").code == command::code::kNotFound);
    CHECK_FALSE(call(R"({"path": "ramp.texture.json", "pixels": [[4, 0]]})").ok);
    CHECK_FALSE(call(R"({"path": "ramp.texture.json", "out": "ramp.jpg"})").ok);

    // Writing a file needs the filesystem capability.
    auto [components2, commands2, reader] = make_session({command::Capability::Read});
    const command::Reply denied =
        reader->call("texture.generate", parse(R"({"path": "ramp.texture.json", "out": "x.png"})"));
    CHECK(denied.code == command::code::kPermissionDenied);
    CHECK(reader->call("texture.generate", parse(R"({"path": "ramp.texture.json"})")).ok);
    std::filesystem::remove_all(root);
}

namespace {

// A world with the built-in components and the canvas installed, on a 32x32 screen.
struct MadeScene {
    MemoryFiles files;
    asset::Assets assets{files.source()};
    world::ComponentRegistry components;
    world::World world{components};

    MadeScene() {
        world::register_builtin_components(components);
        canvas::install(world, assets);
        slash::ui::Settings ui = slash::ui::settings(world);
        ui.viewport = {32.0F, 32.0F};
        slash::ui::set_settings(world, ui);
        canvas::Settings settings;
        settings.clear_color = {.r = 0.0F, .g = 0.0F, .b = 0.0F, .a = 1.0F};
        settings.texture_filter = render::Filter::Nearest;
        canvas::set_settings(world, settings);
    }

    void create(std::string_view id, std::string_view components_json) {
        const entt::entity entity = world.create(id).value();
        const json::Value all = parse(components_json);
        for (const auto& [name, fields] : all.as_object()) {
            std::string path;
            const auto assigned = components.find(name)->assign(world, entity, fields, path);
            REQUIRE_MESSAGE(assigned.ok(), name << path << ": "
                                                << (assigned.ok() ? "" : assigned.error().message));
        }
    }

    canvas::Capture capture() {
        auto captured = canvas::capture(world, assets);
        REQUIRE_MESSAGE(captured.ok(), captured.error().message);
        return std::move(captured).value();
    }
};

render::Image solid(std::uint32_t size, std::vector<std::uint8_t> rgba) {
    render::Image image;
    image.width = size;
    image.height = size;
    for (std::uint32_t i = 0; i < size * size; ++i) {
        image.pixels.insert(image.pixels.end(), rgba.begin(), rgba.end());
    }
    return image;
}

// Two clockwise triangles of a square in the xy plane, facing +z, as Godot's front faces.
void add_square(slash::scene3d::SurfaceTool& tool, float half) {
    const std::array<Vec3, 4> corners{Vec3{-half, -half, 0.0F}, Vec3{half, -half, 0.0F},
                                      Vec3{half, half, 0.0F}, Vec3{-half, half, 0.0F}};
    const std::array<Vec2, 4> uvs{Vec2{0.0F, 1.0F}, Vec2{1.0F, 1.0F}, Vec2{1.0F, 0.0F},
                                  Vec2{0.0F, 0.0F}};
    for (const int i : {0, 2, 1, 0, 3, 2}) {
        tool.set_uv(uvs.at(static_cast<std::size_t>(i)));
        tool.add_vertex(corners.at(static_cast<std::size_t>(i)));
    }
}

} // namespace

TEST_CASE("SurfaceTool builds surfaces as Godot's") {
    using slash::scene3d::SurfaceTool;
    SurfaceTool tool;
    add_square(tool, 1.0F);
    CHECK(tool.vertex_count() == 6);
    // index merges the corners the triangles share.
    tool.index();
    CHECK(tool.vertex_count() == 4);
    CHECK(tool.indices() == std::vector<std::uint32_t>{0, 1, 2, 0, 3, 1});
    // Normals face the viewer of clockwise triangles, here +z; flipped, -z.
    tool.generate_normals();
    tool.generate_tangents();
    CHECK(tool.vertex_count() == 4);
    slash::scene3d::Model model;
    REQUIRE(tool.commit(model).ok());
    CHECK(tool.vertex_count() == 0);
    REQUIRE(model.meshes.size() == 1);
    REQUIRE(model.parts.size() == 1);
    const slash::scene3d::ModelSurface& surface = model.meshes[0].surfaces.at(0);
    CHECK(surface.indices.size() == 6);
    for (const slash::render::Vertex3D& vertex : surface.vertices) {
        CHECK(vertex.normal == std::array<float, 3>{0.0F, 0.0F, 1.0F});
        // MikkTSpace's tangent runs along u, here +x.
        CHECK(vertex.tangent[0] == doctest::Approx(1.0F));
        CHECK(std::abs(vertex.tangent[3]) == 1.0F);
    }
    CHECK(surface.material == -1);

    // A second surface with a material; flipped normals.
    add_square(tool, 0.5F);
    tool.generate_normals(true);
    slash::world::StandardMaterial3D red;
    red.albedo_color = {1.0F, 0.0F, 0.0F, 1.0F};
    tool.set_material(red);
    REQUIRE(tool.commit(model).ok());
    REQUIRE(model.meshes[0].surfaces.size() == 2);
    CHECK(model.meshes[0].surfaces[1].vertices[0].normal ==
          std::array<float, 3>{0.0F, 0.0F, -1.0F});
    CHECK(model.meshes[0].surfaces[1].material == 0);
    CHECK(model.materials.at(0).standard.albedo_color == red.albedo_color);

    // Smooth groups: two faces of a roof meeting at a ridge share normals there; flat ones don't.
    const auto roof = [&](std::uint32_t group) {
        tool.set_smooth_group(group);
        for (const Vec3 p : {Vec3{0, 1, 0}, Vec3{1, 0, -1}, Vec3{1, 0, 1}, Vec3{0, 1, 0},
                             Vec3{-1, 0, 1}, Vec3{-1, 0, -1}}) {
            tool.add_vertex(p);
        }
        tool.generate_normals();
        slash::scene3d::Model out;
        REQUIRE(tool.commit(out).ok());
        return out.meshes[0].surfaces[0].vertices;
    };
    const auto smooth = roof(0);
    CHECK(smooth[0].normal[0] == doctest::Approx(0.0F));
    CHECK(smooth[0].normal[1] == doctest::Approx(1.0F));
    const auto flat = roof(SurfaceTool::kFlat);
    CHECK(flat[0].normal[0] == doctest::Approx(0.7071068F));
    CHECK(flat[3].normal[0] == doctest::Approx(-0.7071068F));

    // Colors are stored as bytes; one vertex is no triangle.
    tool.set_color({1.0F, 0.5F, 0.0F, 1.0F});
    tool.add_vertex({});
    CHECK_FALSE(tool.commit(model).ok());
    tool.clear();
    add_square(tool, 1.0F);
    tool.index();
    tool.add_index(9);
    tool.add_index(0);
    tool.add_index(1);
    const auto bad = tool.commit(model);
    REQUIRE_FALSE(bad.ok());
    CHECK(bad.error().message.find("past the 4 vertices") != std::string::npos);
    CHECK(model.meshes[0].surfaces.size() == 2);
}

TEST_CASE("gameplay code makes textures and meshes that draw") {
    // Renderers draw nothing for a made asset not made yet, and log no warning about it.
    int warnings = 0;
    slash::log::set_sink([&warnings](slash::log::Level level, std::string_view /*line*/) {
        warnings += level == slash::log::Level::Warn ? 1 : 0;
    });
    struct ResetSink {
        ResetSink() = default;
        ResetSink(const ResetSink&) = delete;
        ResetSink& operator=(const ResetSink&) = delete;
        ResetSink(ResetSink&&) = delete;
        ResetSink& operator=(ResetSink&&) = delete;
        ~ResetSink() { slash::log::reset_sink(); }
    } reset_sink;
    MadeScene scene;
    scene.create("sprite", R"({"Sprite2D": {"texture": "made:dot"},
                              "Transform2D": {"position": [8, 8]}})");
    // Not made yet, it draws nothing.
    CHECK(scene.capture().sprites == 0);

    auto path = slash::procedural::make_texture(scene.world, "dot", solid(4, {0, 255, 0, 255}));
    REQUIRE_MESSAGE(path.ok(), path.error().message);
    CHECK(path.value() == "made:dot");
    canvas::Capture captured = scene.capture();
    CHECK(captured.sprites == 1);
    CHECK(pixel(captured.image, 8, 8) == std::vector<int>{0, 255, 0, 255});
    // Made again, the sprite shows the new pixels.
    REQUIRE(slash::procedural::make_texture(scene.world, "dot", solid(4, {0, 0, 255, 255})).ok());
    CHECK(pixel(scene.capture().image, 8, 8) == std::vector<int>{0, 0, 255, 255});
    // A noise texture made with the game's own settings.
    slash::procedural::NoiseTexture2D noise;
    noise.width = 8;
    noise.height = 8;
    noise.noise.seed = 12;
    REQUIRE(
        slash::procedural::make_texture(scene.world, "noise", slash::procedural::make_image(noise))
            .ok());
    CHECK(scene.assets.load<render::Image>("made:noise").value()->width == 8);

    CHECK_FALSE(
        slash::procedural::make_texture(scene.world, "bad name", solid(1, {0, 0, 0, 0})).ok());
    render::Image short_pixels = solid(2, {0, 0, 0, 0});
    short_pixels.pixels.pop_back();
    CHECK_FALSE(slash::procedural::make_texture(scene.world, "short", short_pixels).ok());
    world::ComponentRegistry bare_components;
    world::World bare{bare_components};
    CHECK_FALSE(slash::procedural::make_texture(bare, "dot", solid(1, {0, 0, 0, 0})).ok());

    // A mesh drawn by a MeshInstance3D in front of the camera.
    MadeScene three;
    three.create("camera", R"({"Camera3D": {"current": true},
                               "Transform3D": {"position": [0, 0, 3]}})");
    three.create("square", R"({"MeshInstance3D": {"mesh": "made:square"}, "Transform3D": {}})");
    CHECK(three.capture().meshes == 0);
    slash::scene3d::SurfaceTool tool;
    add_square(tool, 1.0F);
    tool.generate_normals();
    slash::world::StandardMaterial3D red;
    red.albedo_color = {1.0F, 0.0F, 0.0F, 1.0F};
    red.shading_mode = "unshaded";
    tool.set_material(red);
    slash::scene3d::Model mesh;
    REQUIRE(tool.commit(mesh).ok());
    auto made = slash::scene3d::make_mesh(three.world, "square", std::move(mesh));
    REQUIRE_MESSAGE(made.ok(), made.error().message);
    CHECK(made.value() == "made:square");
    captured = three.capture();
    CHECK(captured.meshes == 1);
    const std::vector<int> middle = pixel(captured.image, 16, 16);
    CHECK(middle[0] > 200);
    CHECK(middle[1] < 30);
    CHECK(pixel(captured.image, 1, 1)[0] < 30);
    CHECK(warnings == 0);
}

namespace {

std::vector<procedural::ScatterTriangle> square_ground(float half, float y = 0.0F) {
    const Vec3 a{-half, y, -half};
    const Vec3 b{half, y, -half};
    const Vec3 c{half, y, half};
    const Vec3 d{-half, y, half};
    return {{a, b, c}, {a, c, d}};
}

float distance_across(Vec3 a, Vec3 b) {
    const float dx = a.x - b.x;
    const float dz = a.z - b.z;
    return std::sqrt((dx * dx) + (dz * dz));
}

} // namespace

TEST_CASE("scatter rules are checked with the path of each mistake") {
    const auto errors = [](std::string_view text) {
        return procedural::validate_scatter(parse(text));
    };
    CHECK(errors(R"({"format": 1, "layers": [{"name": "trees", "mesh": "tree.glb",
                     "area": [0, 0, 10, 10], "count": 5}]})")
              .empty());
    const auto first = [&](std::string_view text) {
        const auto found = errors(text);
        REQUIRE_FALSE(found.empty());
        return found.front().path + ": " + found.front().message;
    };
    CHECK(
        first(R"({"format": 1, "layers": [{"name": "a", "mesh": "m.glb", "prefab": "p.prefab.json",
                    "area": [0, 0, 1, 1], "count": 1}]})") ==
        "/layers/0: give mesh or prefab, one of them");
    CHECK(first(R"({"format": 1, "layers": [{"name": "a", "mesh": "m.glb", "area": [0, 0, 1, 1],
                    "count": 1, "density": 2}]})") ==
          "/layers/0: give count or density, one of them");
    CHECK(first(R"({"format": 1, "layers": [{"name": "a", "mesh": "m.glb", "count": 1}]})") ==
          "/layers/0: say where: give on, area or both");
    CHECK(first(R"({"format": 1, "layers": [{"name": "a", "mesh": "m.glb", "count": 1,
                    "area": [2, 0, 1, 1]}]})") ==
          "/layers/0/area: expected min below max on each axis");
    CHECK(first(R"({"format": 1, "layers": [{"name": "a", "prefab": "p.prefab.json",
                    "as": "multimesh", "area": [0, 0, 1, 1], "count": 1}]})") ==
          "/layers/0/as: a multimesh draws a mesh: give mesh");
    CHECK(first(R"({"format": 1, "space": "2d", "layers": [{"name": "a", "mesh": "m.glb",
                    "area": [0, 0, 1, 1], "count": 1}]})") ==
          "/layers/0/mesh: 2D places are entities: give a prefab");
    CHECK(first(R"({"format": 1, "layers": [{"name": "a", "mesh": "m.glb", "on": ["g"],
                    "tiles": ["x"], "count": 1}]})") ==
          "/layers/0/tiles: tiles pick tile map cells, in 2D");
    CHECK(first(R"({"format": 1, "layers": [{"name": "a", "mesh": "m.glb", "on": ["g"],
                    "count": 1, "slope": [40, 10]}]})") == "/layers/0/slope: expected [min, max]");
    CHECK(first(R"({"format": 1, "layers": [
                    {"name": "a", "mesh": "m.glb", "on": ["g"], "count": 1, "avoid": ["b"]},
                    {"name": "b", "mesh": "m.glb", "on": ["g"], "count": 1}]})") ==
          "/layers/0/avoid/0: no earlier layer is named \"b\"");
    CHECK(first(R"({"format": 1, "layers": [
                    {"name": "a", "mesh": "m.glb", "on": ["g"], "count": 1},
                    {"name": "a", "mesh": "m.glb", "on": ["g"], "count": 1}]})") ==
          "/layers/1/name: \"a\" names an earlier layer too");
    CHECK(first(R"({"format": 1, "layers": [{"name": "a b", "mesh": "m.glb", "on": ["g"],
                    "count": 1}]})")
              .starts_with("/layers/0/name"));
    CHECK(first(R"({"format": 1, "layers": [{"name": "a", "mesh": "m.glb", "on": ["g"],
                    "count": 1, "noise": {"frequncy": 1}}]})")
              .starts_with("/layers/0/noise"));
    CHECK_FALSE(procedural::parse_scatter(parse(R"({"format": 2, "layers": []})")).ok());

    const auto rules = procedural::parse_scatter(parse(R"({"format": 1, "seed": 9, "layers": [
        {"name": "trees", "mesh": "tree.glb", "as": "multimesh", "on": ["ground"],
         "area": [-5, -5, 5, 5], "density": 0.5, "spacing": 1.5, "slope": [0, 30],
         "height": [-1, 2], "noise": {"frequency": 0.1}, "noise_range": [-0.2, 1],
         "align_to_normal": true, "rotate_random": 1, "tilt_random": 0.1, "scale": 2,
         "scale_random": 0.5, "offset": -0.25},
        {"name": "rocks", "prefab": "rock.prefab.json", "on": ["ground"], "count": 4,
         "avoid": ["trees"]}]})"));
    REQUIRE_MESSAGE(rules.ok(), rules.error().message);
    CHECK(rules.value().seed == 9);
    CHECK(rules.value().space == "3d");
    const procedural::ScatterLayer& trees = rules.value().layers[0];
    CHECK(trees.as == "multimesh");
    CHECK(trees.area == std::array<float, 4>{-5.0F, -5.0F, 5.0F, 5.0F});
    CHECK(trees.density == 0.5F);
    CHECK(trees.slope == Vec2{0.0F, 30.0F});
    CHECK(trees.height == Vec2{-1.0F, 2.0F});
    CHECK(trees.noise->frequency == doctest::Approx(0.1F));
    CHECK(trees.align_to_normal);
    CHECK(trees.offset == -0.25F);
    CHECK(rules.value().layers[1].count == 4);
    CHECK(rules.value().layers[1].avoid == std::vector<std::string>{"trees"});
}

TEST_CASE("scatter places the same things for the same seed, within its rules") {
    procedural::ScatterLayer layer;
    layer.name = "trees";
    layer.mesh = "tree.glb";
    layer.count = 40;
    layer.rotate_random = 1.0F;
    layer.scale = 2.0F;
    layer.scale_random = 0.5F;
    const auto ground = square_ground(10.0F);
    const procedural::ScatterResult a = procedural::scatter(layer, 0, 7, false, ground, {});
    const procedural::ScatterResult b = procedural::scatter(layer, 0, 7, false, ground, {});
    REQUIRE(a.places.size() == 40);
    CHECK(a.wanted == 40);
    CHECK(a.tries == 40);
    CHECK(a.ground_area == doctest::Approx(400.0F));
    for (std::size_t i = 0; i < a.places.size(); ++i) {
        CHECK(a.places[i].position == b.places[i].position);
        CHECK(a.places[i].rotation == b.places[i].rotation);
        CHECK(a.places[i].scale == b.places[i].scale);
        const procedural::ScatterPlace& place = a.places[i];
        CHECK(std::abs(place.position.x) <= 10.0F);
        CHECK(std::abs(place.position.z) <= 10.0F);
        CHECK(place.position.y == 0.0F);
        CHECK(place.scale >= 1.5F);
        CHECK(place.scale <= 2.5F);
        // Upright, turned only about y.
        CHECK(place.rotation.x == doctest::Approx(0.0F).epsilon(1e-5));
        CHECK(place.rotation.z == doctest::Approx(0.0F).epsilon(1e-5));
    }
    // Another seed, or another layer's stream, places them elsewhere.
    CHECK(procedural::scatter(layer, 0, 8, false, ground, {}).places[0].position !=
          a.places[0].position);
    CHECK(procedural::scatter(layer, 1, 7, false, ground, {}).places[0].position !=
          a.places[0].position);
    // The places are pinned: the same bits on every machine.
    CHECK(a.places[0].position.x == doctest::Approx(-2.365F).epsilon(1e-4));

    // Density times the ground's area, within the area given.
    layer.count.reset();
    layer.density = 0.25F;
    layer.area = std::array<float, 4>{0.0F, 0.0F, 4.0F, 2.0F};
    const procedural::ScatterResult dense = procedural::scatter(layer, 0, 7, false, ground, {});
    CHECK(dense.ground_area == doctest::Approx(8.0F));
    CHECK(dense.places.size() == 2);
    for (const procedural::ScatterPlace& place : dense.places) {
        CHECK(place.position.x >= 0.0F);
        CHECK(place.position.x <= 4.0F);
        CHECK(place.position.z >= 0.0F);
        CHECK(place.position.z <= 2.0F);
    }

    // Spacing keeps places apart, from each other and from those avoided; too much of it leaves
    // the layer short and says why.
    layer.area.reset();
    layer.density.reset();
    layer.count = 60;
    layer.spacing = 1.5F;
    const std::vector<Vec3> avoid{{0.0F, 0.0F, 0.0F}};
    const procedural::ScatterResult apart = procedural::scatter(layer, 0, 7, false, ground, avoid);
    CHECK(apart.places.size() == 60);
    for (std::size_t i = 0; i < apart.places.size(); ++i) {
        CHECK(distance_across(apart.places[i].position, avoid[0]) >= 1.5F);
        for (std::size_t k = 0; k < i; ++k) {
            CHECK(distance_across(apart.places[i].position, apart.places[k].position) >= 1.5F);
        }
    }
    layer.count = 1000;
    layer.spacing = 4.0F;
    const procedural::ScatterResult crowded = procedural::scatter(layer, 0, 7, false, ground, {});
    CHECK(crowded.places.size() < 1000);
    CHECK(crowded.spacing > 0);
    CHECK(crowded.tries == 30000);
}

TEST_CASE("scatter keeps to slopes, heights and noise, and stands on normals") {
    // A flat floor and a ramp at 45 degrees.
    std::vector<procedural::ScatterTriangle> ground = square_ground(5.0F);
    ground.push_back({{10.0F, 0.0F, 0.0F}, {20.0F, 10.0F, 0.0F}, {10.0F, 0.0F, 10.0F}});
    procedural::ScatterLayer layer;
    layer.name = "grass";
    layer.mesh = "grass.glb";
    layer.count = 30;
    layer.slope = {0.0F, 20.0F};
    procedural::ScatterResult flat_only = procedural::scatter(layer, 0, 3, false, ground, {});
    CHECK(flat_only.places.size() == 30);
    CHECK(flat_only.slope > 0);
    for (const procedural::ScatterPlace& place : flat_only.places) {
        CHECK(place.position.x <= 5.0F);
    }
    layer.slope = {40.0F, 50.0F};
    layer.align_to_normal = true;
    const procedural::ScatterResult ramp_only = procedural::scatter(layer, 0, 3, false, ground, {});
    REQUIRE(ramp_only.places.size() == 30);
    for (const procedural::ScatterPlace& place : ramp_only.places) {
        CHECK(place.position.x >= 10.0F);
        // Its up axis is the ramp's normal.
        world::Transform3D t;
        t.rotation = place.rotation;
        const world::Affine3D basis = world::affine(t);
        CHECK(basis.y.x == doctest::Approx(-0.7071F).epsilon(1e-3));
        CHECK(basis.y.y == doctest::Approx(0.7071F).epsilon(1e-3));
        CHECK(basis.y.z == doctest::Approx(0.0F).epsilon(1e-3));
    }
    layer.slope = {0.0F, 90.0F};
    layer.align_to_normal = false;
    layer.height = Vec2{2.0F, 8.0F};
    const procedural::ScatterResult high = procedural::scatter(layer, 0, 3, false, ground, {});
    CHECK(high.height > 0);
    for (const procedural::ScatterPlace& place : high.places) {
        CHECK(place.position.y >= 2.0F);
        CHECK(place.position.y <= 8.0F);
    }

    // Only where the noise is high enough.
    layer.height.reset();
    procedural::NoiseSettings noise;
    noise.frequency = 0.3F;
    layer.noise = noise;
    layer.noise_range = {0.2F, 1.0F};
    const procedural::ScatterResult patchy = procedural::scatter(layer, 0, 3, false, ground, {});
    CHECK(patchy.noise > 0);
    const procedural::Noise sampled(noise);
    for (const procedural::ScatterPlace& place : patchy.places) {
        CHECK(sampled.get_noise_2d(place.position.x, place.position.z) >= 0.2F);
    }

    // In 2D, over x and y, turned in the plane.
    procedural::ScatterLayer flowers;
    flowers.name = "flowers";
    flowers.prefab = "flower.prefab.json";
    flowers.count = 10;
    flowers.rotate_random = 1.0F;
    const std::vector<procedural::ScatterTriangle> rect{
        {{0.0F, 0.0F, 0.0F}, {64.0F, 0.0F, 0.0F}, {64.0F, 32.0F, 0.0F}},
        {{0.0F, 0.0F, 0.0F}, {64.0F, 32.0F, 0.0F}, {0.0F, 32.0F, 0.0F}}};
    const procedural::ScatterResult flat = procedural::scatter(flowers, 0, 3, true, rect, {});
    REQUIRE(flat.places.size() == 10);
    bool turned = false;
    for (const procedural::ScatterPlace& place : flat.places) {
        CHECK(place.position.y >= 0.0F);
        CHECK(place.position.y <= 32.0F);
        CHECK(place.position.z == 0.0F);
        turned = turned || place.rotation.z != 0.0F;
    }
    CHECK(turned);

    // No ground, nothing placed.
    CHECK(procedural::scatter(layer, 0, 3, false, {}, {}).places.empty());
}

TEST_CASE("MultiMeshInstance3D draws its mesh at each instance") {
    MadeScene scene;
    scene.create("camera", R"({"Camera3D": {"current": true, "projection": "orthogonal",
                                            "size": 4},
                               "Transform3D": {"position": [0, 0, 5]}})");
    scene.create("many", R"({"MultiMeshInstance3D": {"mesh": "made:square", "instances": [
                                 {"position": [-1, 0, 0]}, {"position": [1, 0, 0]},
                                 {"position": [0, 1.2, 0], "scale": [0.5, 0.5, 0.5]}]},
                             "Transform3D": {}})");
    slash::scene3d::SurfaceTool tool;
    add_square(tool, 0.4F);
    tool.generate_normals();
    slash::world::StandardMaterial3D red;
    red.albedo_color = {1.0F, 0.0F, 0.0F, 1.0F};
    red.shading_mode = "unshaded";
    tool.set_material(red);
    slash::scene3d::Model mesh;
    REQUIRE(tool.commit(mesh).ok());
    REQUIRE(slash::scene3d::make_mesh(scene.world, "square", std::move(mesh)).ok());
    canvas::Capture captured = scene.capture();
    CHECK(captured.meshes == 3);
    // 4 meters across 32 pixels: x -1 and 1 are pixels 8 and 24, y 1.2 is pixel 6.
    CHECK(pixel(captured.image, 8, 16)[0] > 200);
    CHECK(pixel(captured.image, 24, 16)[0] > 200);
    CHECK(pixel(captured.image, 16, 16)[0] < 30);
    CHECK(pixel(captured.image, 16, 6)[0] > 200);

    // visible_instance_count draws the first ones only.
    scene.world.ecs()
        .get<world::MultiMeshInstance3D>(scene.world.find("many"))
        .visible_instance_count = 1;
    captured = scene.capture();
    CHECK(captured.meshes == 1);
    CHECK(pixel(captured.image, 24, 16)[0] < 30);

    // A mesh that isn't there is a problem at its field, once.
    scene.create("broken", R"({"MultiMeshInstance3D": {"mesh": "tree.png",
                                 "instances": [{}, {}]}, "Transform3D": {}})");
    const auto problems = slash::scene3d::check_meshes(scene.world);
    REQUIRE(problems.size() == 1);
    CHECK(problems[0].path == "/components/MultiMeshInstance3D/mesh");
}

TEST_CASE("scatter.run places what scatter rules ask for") {
    const std::string root = "scatter_test_project";
    std::filesystem::remove_all(root);
    REQUIRE(slash::platform::create_directories(root).ok());
    const auto write = [&](const std::string& file, std::string_view text) {
        REQUIRE(slash::platform::write_file(root + "/" + file, text).ok());
    };
    write("rock.prefab.json", R"({"format": 1, "components": {
        "BoxMesh": {"size": [0.5, 0.5, 0.5]}, "MeshInstance3D": {},
        "Transform3D": {"position": [0, 0.25, 0]}}})");
    write("flower.prefab.json", R"({"format": 1, "components": {
        "Sprite2D": {"texture": "flower.png"}, "Transform2D": {}}})");
    write("ground.tileset.json", R"({"format": 1, "tile_size": [16, 16], "sources": {
        "a": {"texture": "tiles.png", "tiles": {"0,0": {"name": "grass"},
                                                "1,0": {"name": "rock"}}}}})");
    write("yard.scatter.json", R"({"format": 1, "seed": 4, "layers": [
        {"name": "trees", "mesh": "made:tree", "as": "multimesh", "on": ["ground"],
         "count": 12, "spacing": 2, "rotate_random": 1},
        {"name": "rocks", "prefab": "rock.prefab.json", "on": ["ground"], "count": 5,
         "spacing": 1, "avoid": ["trees"], "area": [0, 0, 10, 10]}]})");

    world::ComponentRegistry components;
    world::register_builtin_components(components);
    command::Registry commands;
    command::register_builtin_commands(commands);
    command::SessionOptions options;
    options.project_root = root;
    options.capture_logs = false;
    command::Session session(components, commands, options);
    world::World& world = session.world();
    const auto call = [&](std::string_view name, std::string_view params) {
        return session.call(name, parse(params));
    };
    REQUIRE(call("entity.create", R"({"id": "ground", "components": {
        "PlaneMesh": {"size": [20, 20]}, "MeshInstance3D": {}, "Transform3D": {}}})")
                .ok);

    command::Reply reply = call("scatter.run", R"({"path": "yard.scatter.json", "into": "yard"})");
    REQUIRE_MESSAGE(reply.ok, reply.message);
    CHECK(reply.value == parse(R"({"into": "yard", "layers": [{"name": "trees", "placed": 12},
                                                             {"name": "rocks", "placed": 5}]})"));
    const entt::entity yard = world.find("yard");
    REQUIRE(world.valid(yard));
    const auto& children = world.ecs().get<world::Transform3D>(yard).children;
    REQUIRE(children.size() == 6);
    CHECK(world.id_of(children[0]) == "yard_trees");
    CHECK(world.id_of(children[1]) == "yard_rocks_1");
    const auto& trees = world.ecs().get<world::MultiMeshInstance3D>(children[0]);
    CHECK(trees.mesh == "made:tree");
    REQUIRE(trees.instances.size() == 12);
    std::vector<Vec3> tree_places;
    for (const world::MultiMeshInstance& instance : trees.instances) {
        CHECK(std::abs(instance.position.x) <= 10.0F);
        CHECK(std::abs(instance.position.z) <= 10.0F);
        CHECK(instance.position.y == doctest::Approx(0.0F).epsilon(1e-4));
        tree_places.push_back(instance.position);
    }
    // Rocks extend their prefab, in the area, apart from the trees.
    for (std::size_t i = 1; i < children.size(); ++i) {
        REQUIRE(world.prefab_of(children[i]) != nullptr);
        CHECK(world.prefab_of(children[i])->path == "rock.prefab.json");
        CHECK(world.ecs().all_of<world::BoxMesh>(children[i]));
        const Vec3 at = world.ecs().get<world::Transform3D>(children[i]).position;
        CHECK(at.x >= 0.0F);
        CHECK(at.z <= 10.0F);
        for (const Vec3 tree : tree_places) {
            CHECK(distance_across(at, tree) >= 1.0F);
        }
    }

    // Running again replaces what the run before made, and is one undo step; the old rocks
    // aren't ground. A parent moved places them in its space.
    REQUIRE(call("component.set", R"({"id": "yard", "components": {
        "Transform3D": {"position": [5, 0, 0]}}})")
                .ok);
    reply = call("scatter.run", R"({"path": "yard.scatter.json", "into": "yard"})");
    REQUIRE_MESSAGE(reply.ok, reply.message);
    CHECK(world.ecs().get<world::Transform3D>(yard).children.size() == 6);
    CHECK_FALSE(world.valid(world.find("yard_rocks_6")));
    const auto& moved = world.ecs().get<world::MultiMeshInstance3D>(world.find("yard_trees"));
    CHECK(moved.instances[0].position.x == doctest::Approx(tree_places[0].x - 5.0F));
    REQUIRE(call("undo", "{}").ok);
    CHECK(world.ecs()
              .get<world::MultiMeshInstance3D>(world.find("yard_trees"))
              .instances[0]
              .position == tree_places[0]);

    // Inline rules; a layer that can't fill up says why.
    reply = call("scatter.run", R"({"into": "crowd", "rules": {"format": 1, "layers": [
        {"name": "posts", "mesh": "made:tree", "area": [0, 0, 2, 2], "count": 50,
         "spacing": 1}]}})");
    REQUIRE_MESSAGE(reply.ok, reply.message);
    const json::Value& posts = reply.value.find("layers")->as_array()[0];
    CHECK(posts.find("placed")->as_int() < 50);
    CHECK(posts.find("wanted")->as_int() == 50);
    CHECK(posts.find("rejected")->find("spacing")->as_int() > 0);
    CHECK(world.ecs().get<world::MeshInstance3D>(world.find("crowd_posts_1")).mesh == "made:tree");

    // A dry run changes nothing.
    const std::size_t before = world.ids().size();
    reply = call("scatter.run", R"({"into": "dry", "dry_run": true, "rules": {"format": 1,
        "layers": [{"name": "a", "mesh": "made:tree", "area": [0, 0, 2, 2], "count": 3}]}})");
    REQUIRE_MESSAGE(reply.ok, reply.message);
    CHECK(world.ids().size() == before);
    CHECK_FALSE(world.valid(world.find("dry")));

    // 2D: flowers on the grass cells of a tile map.
    REQUIRE(call("entity.create", R"({"id": "map", "components": {
        "TileMapLayer": {"tile_set": "ground.tileset.json", "legend": {"g": "grass", "r": "rock"},
                         "rows": ["ggrr", "ggrr"]},
        "Transform2D": {"position": [100, 0]}}})")
                .ok);
    reply = call("scatter.run", R"({"into": "meadow", "rules": {"format": 1, "space": "2d",
        "layers": [{"name": "flowers", "prefab": "flower.prefab.json", "on": ["map"],
                    "tiles": ["grass"], "count": 8, "rotate_random": 1}]}})");
    REQUIRE_MESSAGE(reply.ok, reply.message);
    const auto& flowers = world.ecs().get<world::Transform2D>(world.find("meadow")).children;
    REQUIRE(flowers.size() == 8);
    for (const entt::entity flower : flowers) {
        const Vec2 at = world.ecs().get<world::Transform2D>(flower).position;
        CHECK(at.x >= 100.0F);
        CHECK(at.x <= 132.0F);
        CHECK(at.y >= 0.0F);
        CHECK(at.y <= 32.0F);
        CHECK(world.ecs().all_of<world::Sprite2D>(flower));
    }

    // Mistakes point at what's wrong.
    reply = call("scatter.run", R"({"into": "x", "rules": {"format": 1, "layers": [
        {"name": "a", "mesh": "m.glb", "on": ["grond"], "count": 1}]}})");
    CHECK_FALSE(reply.ok);
    CHECK(reply.message.find("/rules/layers/0/on/0") != std::string::npos);
    CHECK(reply.message.find("did you mean \"ground\"") != std::string::npos);
    reply = call("scatter.run", R"({"into": "x", "rules": {"format": 1, "layers": [
        {"name": "a", "count": 1, "area": [0, 0, 1, 1]}]}})");
    CHECK_FALSE(reply.ok);
    CHECK(reply.message.find("/rules/layers/0") != std::string::npos);
    CHECK_FALSE(call("scatter.run", R"({"into": "x", "path": "yard.json"})").ok);
    CHECK_FALSE(call("scatter.run", R"({"into": "x"})").ok);
    CHECK_FALSE(call("scatter.run", R"({"into": "map", "path": "yard.scatter.json"})").ok);
    reply = call("scatter.run", R"({"into": "y", "rules": {"format": 1, "space": "2d",
        "layers": [{"name": "a", "prefab": "flower.prefab.json", "on": ["ground"],
                    "count": 1}]}})");
    CHECK_FALSE(reply.ok);
    CHECK(reply.message.find("has no TileMapLayer") != std::string::npos);
    std::filesystem::remove_all(root);
}
