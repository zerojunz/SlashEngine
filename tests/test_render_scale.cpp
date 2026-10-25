// The render scale (render/upscaler.hpp) in the canvas renderer, project.json and the commands:
// the world drawn smaller or larger and scaled to the screen, under controls drawn at full size.

#include "slash/asset/assets.hpp"
#include "slash/canvas/canvas.hpp"
#include "slash/command/builtin.hpp"
#include "slash/command/command.hpp"
#include "slash/command/session.hpp"
#include "slash/core/json.hpp"
#include "slash/platform/file.hpp"
#include "slash/platform/platform.hpp"
#include "slash/platform/run.hpp"
#include "slash/render/device.hpp"
#include "slash/render/image.hpp"
#include "slash/render/upscaler.hpp"
#include "slash/runtime/project.hpp"
#include "slash/ui/ui.hpp"
#include "slash/world/component_registry.hpp"
#include "slash/world/components.hpp"
#include "slash/world/world.hpp"

#include <doctest/doctest.h>

#include <array>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace asset = slash::asset;
namespace canvas = slash::canvas;
namespace command = slash::command;
namespace json = slash::json;
namespace render = slash::render;
namespace world = slash::world;

namespace {

json::Value parse(std::string_view text) {
    auto parsed = json::parse(text);
    REQUIRE_MESSAGE(parsed.ok(), (parsed.ok() ? std::string() : parsed.error().message));
    return std::move(parsed).value();
}

bool contains(std::string_view text, std::string_view part) {
    return text.find(part) != std::string_view::npos;
}

// Files in memory, for a world's assets.
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

using Rgba = std::array<std::uint8_t, 4>;

std::string square_png(Rgba color, std::uint32_t size) {
    render::Image image{.width = size, .height = size, .pixels = {}};
    for (std::uint32_t i = 0; i < size * size; ++i) {
        image.pixels.insert(image.pixels.end(), color.begin(), color.end());
    }
    return render::encode_png(image).value();
}

Rgba pixel(const render::Image& image, std::uint32_t x, std::uint32_t y) {
    const std::size_t at = ((std::size_t{y} * image.width) + x) * 4;
    return {image.pixels[at], image.pixels[at + 1], image.pixels[at + 2], image.pixels[at + 3]};
}

constexpr Rgba kBlack{0, 0, 0, 255};
constexpr Rgba kRed{255, 0, 0, 255};
constexpr Rgba kWhite{255, 255, 255, 255};

// A 16x8 screen: a white 4x4 sprite at (4, 0) in the world, a red control over (10, 0) to
// (14, 3), a green sprite at (12, 4) on a canvas layer below the world and a white one at (0, 4)
// on a layer above it.
struct Scene {
    MemoryFiles files;
    asset::Assets assets{files.source()};
    world::ComponentRegistry components;
    world::World world{components};

    Scene() {
        world::register_builtin_components(components);
        canvas::install(world, assets);
        slash::ui::Settings ui = slash::ui::settings(world);
        ui.viewport = {16.0F, 8.0F};
        slash::ui::set_settings(world, ui);
        files.files["white.png"] = square_png(kWhite, 4);
        files.files["green.png"] = square_png({0, 255, 0, 255}, 4);
        create("box", R"({"Transform2D": {"position": [4, 0]},
                          "Sprite2D": {"texture": "white.png", "centered": false}})");
        create("bar", R"({"Control": {"offset_left": 10, "offset_right": 14, "offset_bottom": 3},
                          "ColorRect": {"color": [1, 0, 0, 1]}})");
        create("back_sprite", R"({"Transform2D": {"position": [12, 4]},
                                  "Sprite2D": {"texture": "green.png", "centered": false}})");
        create("back", R"({"CanvasLayer": {"layer": -1, "children": ["back_sprite"]}})");
        create("top_sprite", R"({"Transform2D": {"position": [0, 4]},
                                 "Sprite2D": {"texture": "white.png", "centered": false}})");
        create("top", R"({"CanvasLayer": {"layer": 1, "children": ["top_sprite"]}})");
        scale({});
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

    void scale(const render::Scaling& scaling) {
        canvas::Settings settings;
        settings.clear_color = {.r = 0.0F, .g = 0.0F, .b = 0.0F, .a = 1.0F};
        settings.texture_filter = render::Filter::Nearest;
        settings.scaling = scaling;
        canvas::set_settings(world, settings);
    }

    canvas::Capture capture() {
        auto captured = canvas::capture(world, assets);
        REQUIRE_MESSAGE(captured.ok(), (captured.ok() ? "" : captured.error().message));
        return std::move(captured).value();
    }
};

// Checks a pixel's color within a tolerance a channel.
void check_pixel(const render::Image& image, std::uint32_t x, std::uint32_t y, Rgba want,
                 int tolerance = 0) {
    const Rgba got = pixel(image, x, y);
    CAPTURE(x);
    CAPTURE(y);
    for (std::size_t c = 0; c < 4; ++c) {
        CAPTURE(c);
        CAPTURE(int{got.at(c)});
        CHECK(std::abs(int{got.at(c)} - int{want.at(c)}) <= tolerance);
    }
}

// What an added upscaler was asked to do, for the test that counts its calls.
struct Calls {
    int made = 0;
    int inputs = 0;
    int upscales = 0;
    int released = 0;
    bool runs = true;
};
Calls& calls() {
    static Calls instance;
    return instance;
}

// Stretches the render with no filtering, counting its calls.
class Counting final : public render::Upscaler {
public:
    Counting() { ++calls().made; }

    slash::Result<render::TextureId> input(render::RenderDevice& device,
                                           const render::UpscaleSize& size) override {
        ++calls().inputs;
        if (texture_ == render::TextureId::None) {
            auto made = device.create_texture({.width = size.render_width,
                                               .height = size.render_height,
                                               .filter = render::Filter::Nearest,
                                               .render_target = true});
            if (!made) {
                return made.error();
            }
            texture_ = made.value();
        }
        return texture_;
    }

    slash::Result<render::UpscaleDraw> upscale(render::RenderDevice& /*device*/,
                                               const render::UpscaleSize& /*size*/,
                                               const render::Scaling& /*scaling*/) override {
        ++calls().upscales;
        return render::UpscaleDraw{.texture = texture_, .material = {}};
    }

    void release(render::RenderDevice& device) override {
        ++calls().released;
        if (texture_ != render::TextureId::None) {
            device.destroy_texture(texture_);
            texture_ = render::TextureId::None;
        }
    }

private:
    render::TextureId texture_ = render::TextureId::None;
};

} // namespace

TEST_CASE("the world draws at the render scale under controls and layers drawn at full size") {
    Scene scene;
    const canvas::Capture native = scene.capture();
    CHECK(native.scaled.upscaler.empty());
    check_pixel(native.image, 3, 1, kBlack);
    check_pixel(native.image, 4, 1, kWhite);
    check_pixel(native.image, 11, 5, kBlack);
    check_pixel(native.image, 12, 5, {0, 255, 0, 255});

    // Bilinear at a scale of 1 is no scaling at all.
    scene.scale({.mode = "bilinear", .scale = 1.0F, .fsr_sharpness = 0.2F, .upscaler = ""});
    const canvas::Capture same = scene.capture();
    CHECK(same.scaled.upscaler.empty());
    CHECK(same.image.pixels == native.image.pixels);

    SUBCASE("bilinear at half size stretches the world, filtered") {
        scene.scale({.mode = "bilinear", .scale = 0.5F, .fsr_sharpness = 0.2F, .upscaler = ""});
        const canvas::Capture half = scene.capture();
        CHECK(half.scaled.upscaler == "bilinear");
        CHECK(half.scaled.size ==
              render::UpscaleSize{
                  .render_width = 8, .render_height = 4, .output_width = 16, .output_height = 8});
        // The sprite's left edge falls between texels 1 and 2 of the 8x4 render: a quarter and
        // three quarters of the way to white.
        check_pixel(half.image, 3, 1, {64, 64, 64, 255}, 1);
        check_pixel(half.image, 4, 1, {191, 191, 191, 255}, 1);
        // The layer below the world is scaled with it; the controls and the layer above aren't.
        check_pixel(half.image, 11, 5, {0, 64, 0, 255}, 1);
        check_pixel(half.image, 12, 5, {0, 191, 0, 255}, 1);
        check_pixel(half.image, 9, 1, kBlack);
        check_pixel(half.image, 10, 1, kRed);
        check_pixel(half.image, 3, 5, kWhite);
        check_pixel(half.image, 4, 5, kBlack);
        CHECK(half.sprites == native.sprites);
        CHECK(half.controls == native.controls);
    }

    SUBCASE("FSR at half size keeps the edge sharper than bilinear") {
        scene.scale({.mode = "bilinear", .scale = 0.5F, .fsr_sharpness = 0.2F, .upscaler = ""});
        const render::Image bilinear = scene.capture().image;
        scene.scale({.mode = "fsr", .scale = 0.5F, .fsr_sharpness = 0.2F, .upscaler = ""});
        const canvas::Capture fsr = scene.capture();
        CHECK(fsr.scaled.upscaler == "fsr");
        CHECK(fsr.scaled.size.render_width == 8);
        CHECK(pixel(fsr.image, 3, 1)[0] < pixel(bilinear, 3, 1)[0]);
        CHECK(pixel(fsr.image, 4, 1)[0] > pixel(bilinear, 4, 1)[0]);
        check_pixel(fsr.image, 9, 1, kBlack);
        check_pixel(fsr.image, 10, 1, kRed);
        check_pixel(fsr.image, 3, 5, kWhite);
        // A softer sharpness (2 stops) changes what RCAS draws.
        scene.scale({.mode = "fsr", .scale = 0.5F, .fsr_sharpness = 2.0F, .upscaler = ""});
        const render::Image soft = scene.capture().image;
        CHECK(soft.pixels != fsr.image.pixels);
    }

    SUBCASE("above 1 the world draws larger and is averaged down, by bilinear even for FSR") {
        scene.scale({.mode = "fsr", .scale = 2.0F, .fsr_sharpness = 0.2F, .upscaler = ""});
        const canvas::Capture twice = scene.capture();
        CHECK(twice.scaled.upscaler == "bilinear");
        CHECK(twice.scaled.size.render_width == 32);
        CHECK(twice.scaled.size.render_height == 16);
        // Every edge is on an even texel of the 32x16 render, so each pair averages to the
        // native pixel.
        CHECK(twice.image.pixels == native.image.pixels);
    }

    SUBCASE("an upscaler the game doesn't have leaves the mode's") {
        scene.scale({.mode = "fsr", .scale = 0.5F, .fsr_sharpness = 0.2F, .upscaler = "dlss"});
        CHECK(scene.capture().scaled.upscaler == "fsr");
        scene.scale({.mode = "bilinear", .scale = 1.0F, .fsr_sharpness = 0.2F, .upscaler = "dlss"});
        CHECK(scene.capture().image.pixels == native.image.pixels);
    }
}

TEST_CASE("an added upscaler is made once, used where it runs and released") {
    render::clear_added_upscalers();
    calls() = {};
    REQUIRE(
        render::add_upscaler(
            {.name = "counting",
             .temporal = false,
             .supports = [](const render::RenderDevice&) { return calls().runs; },
             .make = [] { return std::unique_ptr<render::Upscaler>(std::make_unique<Counting>()); },
             .make_temporal = {}})
            .ok());
    Scene scene;
    scene.scale({.mode = "bilinear", .scale = 0.5F, .fsr_sharpness = 0.2F, .upscaler = "counting"});
    const std::unique_ptr<render::RenderDevice> device = render::create_software_device();
    const render::TextureId target = device
                                         ->create_texture({.width = 16,
                                                           .height = 8,
                                                           .filter = render::Filter::Nearest,
                                                           .render_target = true})
                                         .value();
    const canvas::CanvasOptions options{.target = target,
                                        .clear = {.r = 0.0F, .g = 0.0F, .b = 0.0F, .a = 1.0F},
                                        .width = 16.0F,
                                        .height = 8.0F};
    {
        canvas::CanvasRenderer renderer(*device, scene.assets);
        REQUIRE(renderer.draw(scene.world, options).ok());
        REQUIRE(renderer.draw(scene.world, options).ok());
        CHECK(renderer.scaled().upscaler == "counting");
        CHECK(renderer.scaled().size.render_width == 8);
        CHECK(calls().made == 1);
        CHECK(calls().inputs == 2);
        CHECK(calls().upscales == 2);
        CHECK(calls().released == 0);
        // No filtering: the sprite's left edge stays one step, two pixels from where it was.
        const render::Image image = device->read_texture(target).value();
        check_pixel(image, 3, 1, kBlack);
        check_pixel(image, 4, 1, kWhite);

        // Where it doesn't run (a vendor's on another vendor's GPU), the mode's takes over.
        calls().runs = false;
        REQUIRE(renderer.draw(scene.world, options).ok());
        CHECK(renderer.scaled().upscaler == "bilinear");
        CHECK(calls().released == 1);
        calls().runs = true;
        REQUIRE(renderer.draw(scene.world, options).ok());
        CHECK(calls().made == 2);
    }
    // The renderer releases the one it has when it goes.
    CHECK(calls().released == 2);
    render::clear_added_upscalers();
}

TEST_CASE("project.json's rendering.scaling is read and checked") {
    json::Value project = parse(R"({"format": 1, "name": "Scaled", "version": "1.0.0",
        "main_scene": "main.scene.json",
        "rendering": {"scaling": {"mode": "fsr", "scale": 0.5, "fsr_sharpness": 1}}})");
    CHECK(slash::runtime::validate_project(project).empty());
    const slash::runtime::Project parsed = slash::runtime::parse_project(project).value();
    CHECK(parsed.scaling ==
          render::Scaling{.mode = "fsr", .scale = 0.5F, .fsr_sharpness = 1.0F, .upscaler = ""});
    // The default is no scaling.
    json::Value plain = project;
    plain["rendering"] = json::Value(json::Object{});
    CHECK(slash::runtime::parse_project(plain).value().scaling == render::Scaling{});

    const auto first_error = [&](std::string_view scaling) {
        project["rendering"]["scaling"] = parse(scaling);
        const auto errors = slash::runtime::validate_project(project);
        REQUIRE_FALSE(errors.empty());
        return errors.front();
    };
    project["rendering"]["scaling"] = parse(R"({"mode": "fsr2", "scale": 0.67})");
    CHECK(slash::runtime::validate_project(project).empty());
    // Godot's MetalFX modes are valid everywhere: where MetalFX isn't, they fall back.
    project["rendering"]["scaling"] = parse(R"({"mode": "metalfx_temporal", "scale": 0.67})");
    CHECK(slash::runtime::validate_project(project).empty());
    auto error = first_error(R"({"mode": "metalfx_temporl"})");
    CHECK(error.path == "/rendering/scaling/mode");
    CHECK(contains(error.message, "did you mean metalfx_temporal?"));
    error = first_error(R"({"mode": "bilinar"})");
    CHECK(error.path == "/rendering/scaling/mode");
    CHECK(contains(error.message, "did you mean bilinear?"));
    CHECK(first_error(R"({"scale": 3})").path == "/rendering/scaling/scale");
    CHECK(first_error(R"({"fsr_sharpness": -1})").path == "/rendering/scaling/fsr_sharpness");
    CHECK(first_error(R"({"upscaler": "Big Upscaler"})").path == "/rendering/scaling/upscaler");
    CHECK(first_error(R"({"mood": "fsr"})").path == "/rendering/scaling/mood");
    CHECK(slash::runtime::parse_project(project).error().code == slash::ErrorCode::InvalidArgument);
}

TEST_CASE("display.set changes the render scale, and frame.capture and display.list show it") {
    const std::string root = "render_scale_project";
    std::filesystem::remove_all(root);
    REQUIRE(slash::platform::create_directories(root).ok());
    REQUIRE(slash::platform::write_file(root + "/project.json", R"({"format": 1,
        "name": "Scaled", "main_scene": "main.scene.json", "version": "1.0.0",
        "window": {"width": 16, "height": 8},
        "rendering": {"clear_color": [0, 0, 0, 1], "texture_filter": "nearest",
                      "scaling": {"mode": "fsr", "scale": 0.5}}})")
                .ok());
    REQUIRE(slash::platform::write_file(root + "/white.png", square_png(kWhite, 4)).ok());
    REQUIRE(slash::platform::write_file(root + "/main.scene.json", R"({"format": 1,
        "entities": {"box": {"components": {"Transform2D": {"position": [4, 0]},
            "Sprite2D": {"texture": "white.png", "centered": false}}}}})")
                .ok());

    auto platform = slash::platform::Platform::create({.headless = true});
    REQUIRE(platform.ok());
    auto loop =
        slash::platform::Loop::create(platform.value(), {.tick_hz = 60.0, .max_ticks = 600});
    REQUIRE(loop.ok());
    const auto null_device = render::create_null_device();
    const std::unique_ptr<render::RenderDevice> device = render::create_software_device();
    world::ComponentRegistry components;
    world::register_builtin_components(components);
    command::Registry commands;
    command::register_builtin_commands(commands);
    command::SessionOptions options;
    options.project_root = root;
    options.capture_logs = false;
    {
        command::Session session(components, commands, options);
        canvas::CanvasRenderer renderer(*device, session.assets());
        command::register_live_commands(commands, {.platform = &platform.value(),
                                                   .loop = &loop.value(),
                                                   .renderer = null_device.get(),
                                                   .canvas = &renderer});
        const auto ok = [&](std::string_view name, std::string_view params) {
            command::Reply reply = session.call(name, parse(params));
            REQUIRE_MESSAGE(reply.ok, reply.message);
            return std::move(reply.value);
        };
        ok("scene.open", R"({"path": "main.scene.json"})");

        // The project's settings, and what the game's last frame drew with: none yet.
        CHECK(*ok("display.list", "{}").find("scaling") ==
              parse(R"({"fsr_sharpness": 0.2, "in_effect": "none", "mode": "fsr",
                        "scale": 0.5})"));
        json::Value captured = ok("frame.capture", R"({"points": [[3, 1], [4, 1]]})");
        CHECK(captured.find("upscaler")->as_string() == "fsr");
        CHECK(*captured.find("render_size") == parse("[8, 4]"));

        // The game draws a frame: display.list says what it scaled with.
        const render::TextureId target = device
                                             ->create_texture({.width = 16,
                                                               .height = 8,
                                                               .filter = render::Filter::Nearest,
                                                               .render_target = true})
                                             .value();
        REQUIRE(renderer.draw(session.world(), {.target = target, .width = 16.0F, .height = 8.0F})
                    .ok());
        CHECK(*ok("display.list", "{}").find("scaling") ==
              parse(R"({"fsr_sharpness": 0.2, "in_effect": "fsr", "mode": "fsr", "scale": 0.5,
                        "render_size": [8, 4]})"));

        // Fields left out stay as they were.
        const json::Value set = ok("display.set", R"({"scaling": {"mode": "bilinear"}})");
        CHECK(set.find("scaling")->find("mode")->as_string() == "bilinear");
        CHECK(set.find("scaling")->find("scale")->as_double() == doctest::Approx(0.5));
        CHECK(canvas::settings(session.world()).scaling.mode == "bilinear");
        captured = ok("frame.capture", R"({"points": [[3, 1], [4, 1]]})");
        CHECK(captured.find("upscaler")->as_string() == "bilinear");
        CHECK(*captured.find("colors") ==
              parse("[[0.251, 0.251, 0.251, 1], [0.749, 0.749, 0.749, 1]]"));
        // fsr2 scales only the 3D world: a world without one scales with bilinear, as Godot's
        // falls back.
        ok("display.set", R"({"scaling": {"mode": "fsr2"}})");
        captured = ok("frame.capture", R"({"points": [[3, 1], [4, 1]]})");
        CHECK(captured.find("upscaler")->as_string() == "bilinear");
        // MetalFX's modes fall back as Godot's do where MetalFX isn't: the spatial one to FSR, and
        // the temporal one to FSR 2, which here is bilinear too.
        ok("display.set", R"({"scaling": {"mode": "metalfx_spatial"}})");
        captured = ok("frame.capture", R"({"points": [[3, 1], [4, 1]]})");
        CHECK(captured.find("upscaler")->as_string() == "fsr");
        ok("display.set", R"({"scaling": {"mode": "metalfx_temporal"}})");
        captured = ok("frame.capture", R"({"points": [[3, 1], [4, 1]]})");
        CHECK(captured.find("upscaler")->as_string() == "bilinear");
        CHECK(*captured.find("colors") ==
              parse("[[0.251, 0.251, 0.251, 1], [0.749, 0.749, 0.749, 1]]"));
        ok("display.set", R"({"scaling": {"mode": "bilinear", "scale": 1}})");
        captured = ok("frame.capture", R"({"points": [[3, 1], [4, 1]]})");
        CHECK(captured.find("upscaler") == nullptr);
        CHECK(*captured.find("colors") == parse("[[0, 0, 0, 1], [1, 1, 1, 1]]"));

        // Bad settings change nothing.
        command::Reply bad =
            session.call("display.set", parse(R"({"scaling": {"mode": "metalfx_spatail"}})"));
        CHECK(bad.code == command::code::kInvalidArgument);
        CHECK(contains(bad.message, "/scaling/mode"));
        CHECK(contains(bad.message, "did you mean metalfx_spatial?"));
        bad = session.call("display.set",
                           parse(R"({"scaling": {"upscaler": "fsrr"}, "max_fps": 30})"));
        CHECK(bad.code == command::code::kNotFound);
        CHECK(contains(bad.message, "it has bilinear, fsr, metalfx_spatial (did you mean fsr?)"));
        CHECK(loop.value().max_fps() != 30);
        bad = session.call("display.set", parse(R"({"scaling": {"scale": 0.1}})"));
        CHECK(bad.code == command::code::kInvalidParams);
        CHECK(contains(bad.message, "/scaling/scale"));
        CHECK(canvas::settings(session.world()).scaling ==
              render::Scaling{
                  .mode = "bilinear", .scale = 1.0F, .fsr_sharpness = 0.2F, .upscaler = ""});
    }
    std::filesystem::remove_all(root);
}
