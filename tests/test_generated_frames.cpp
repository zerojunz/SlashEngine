// Frame generation in the canvas renderer, project.json and the display commands: a frame made
// between each two drawn ones (RenderDevice::generate_frame) shown first, the UI over both.

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
#include <vector>

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

// A gray picture of 4 x 4 pixel cells from an integer hash, so every block of 8 x 8 pixels looks
// unlike its neighbors and the optical flow can find it.
std::string pattern_png(std::uint32_t size) {
    render::Image image{.width = size, .height = size, .pixels = {}};
    for (std::uint32_t y = 0; y < size; ++y) {
        for (std::uint32_t x = 0; x < size; ++x) {
            std::uint32_t h = ((x >> 2U) * 73856093U) ^ ((y >> 2U) * 19349663U);
            h ^= h >> 13U;
            h *= 0x5bd1e995U;
            h ^= h >> 15U;
            const auto value = static_cast<std::uint8_t>(h & 0xFFU);
            image.pixels.insert(image.pixels.end(), {value, value, value, 255});
        }
    }
    return render::encode_png(image).value();
}

// How many channels of a and b differ by more than by.
std::size_t differing(const render::Image& a, const render::Image& b, int by) {
    std::size_t count = 0;
    for (std::size_t i = 0; i < a.pixels.size(); ++i) {
        count += std::abs(int{a.pixels[i]} - int{b.pixels[i]}) > by ? 1U : 0U;
    }
    return count;
}

// Whether the pixels from (x0, y0) up to (x1, y1) are all red.
bool red(const render::Image& image, std::uint32_t x0, std::uint32_t y0, std::uint32_t x1,
         std::uint32_t y1) {
    for (std::uint32_t y = y0; y < y1; ++y) {
        for (std::uint32_t x = x0; x < x1; ++x) {
            const std::size_t at = ((std::size_t{y} * image.width) + x) * 4;
            if (image.pixels[at] != 255 || image.pixels[at + 1] != 0 || image.pixels[at + 2] != 0) {
                return false;
            }
        }
    }
    return true;
}

// A 64x64 screen: the pattern as a sprite 64 pixels across, which the test slides right, under
// a red control from (40, 4) to (60, 12).
struct Scene {
    MemoryFiles files;
    asset::Assets assets{files.source()};
    world::ComponentRegistry components;
    world::World world{components};
    entt::entity picture = entt::null;

    explicit Scene(bool frame_generation) {
        world::register_builtin_components(components);
        canvas::install(world, assets);
        slash::ui::Settings ui = slash::ui::settings(world);
        ui.viewport = {64.0F, 64.0F};
        slash::ui::set_settings(world, ui);
        files.files["pattern.png"] = pattern_png(64);
        picture = create("picture", R"({"Transform2D": {},
            "Sprite2D": {"texture": "pattern.png", "centered": false}})");
        create("bar", R"({"Control": {"offset_left": 40, "offset_top": 4, "offset_right": 60,
                                      "offset_bottom": 12},
                          "ColorRect": {"color": [1, 0, 0, 1]}})");
        canvas::Settings settings;
        settings.clear_color = {.r = 0.0F, .g = 0.0F, .b = 0.0F, .a = 1.0F};
        settings.texture_filter = render::Filter::Nearest;
        settings.frame_generation = frame_generation;
        canvas::set_settings(world, settings);
    }

    entt::entity create(std::string_view id, std::string_view components_json) {
        const entt::entity entity = world.create(id).value();
        const json::Value all = parse(components_json);
        for (const auto& [name, fields] : all.as_object()) {
            std::string path;
            const auto assigned = components.find(name)->assign(world, entity, fields, path);
            REQUIRE_MESSAGE(assigned.ok(), name << path << ": "
                                                << (assigned.ok() ? "" : assigned.error().message));
        }
        return entity;
    }

    void move_to(float x) { world.ecs().get<world::Transform2D>(picture).position = {x, 0.0F}; }
};

} // namespace

TEST_CASE("A generated frame shows before each drawn one, the controls drawn over both") {
    const std::unique_ptr<render::RenderDevice> device = render::create_software_device();
    const auto target = device->create_texture(
        {.width = 64, .height = 64, .filter = render::Filter::Nearest, .render_target = true});
    REQUIRE(target.ok());
    Scene scene(true);
    canvas::CanvasRenderer renderer(*device, scene.assets);
    std::vector<render::Image> generated;
    canvas::CanvasOptions options{.target = target.value(), .width = 64.0F, .height = 64.0F};
    options.generate = true;
    options.between_frames = [&] {
        auto shown = device->read_texture(target.value());
        REQUIRE(shown.ok());
        generated.push_back(std::move(shown).value());
    };
    // The picture moves 4 pixels right a frame. With nothing in 3D, the optical flow carries
    // it, once ten frames have passed since its first, which count as scene changes.
    constexpr int kFrames = 24;
    const auto x_of = [](int frame) { return static_cast<float>(frame * 4) - 96.0F; };
    for (int frame = 0; frame < kFrames; ++frame) {
        scene.move_to(x_of(frame));
        REQUIRE(renderer.draw(scene.world, options).ok());
        CHECK(renderer.generated());
    }
    REQUIRE(generated.size() == kFrames);
    CHECK(renderer.frames_generated() == kFrames);
    auto drawn = device->read_texture(target.value());
    REQUIRE(drawn.ok());

    // The same frames drawn without frame generation, by another renderer.
    canvas::CanvasRenderer plain(*device, scene.assets);
    const auto plain_target = device->create_texture(
        {.width = 64, .height = 64, .filter = render::Filter::Nearest, .render_target = true});
    REQUIRE(plain_target.ok());
    const auto draw_plain = [&](float x) {
        scene.move_to(x);
        REQUIRE(plain
                    .draw(scene.world,
                          {.target = plain_target.value(), .width = 64.0F, .height = 64.0F})
                    .ok());
        CHECK_FALSE(plain.generated());
        auto image = device->read_texture(plain_target.value());
        REQUIRE(image.ok());
        return std::move(image).value();
    };
    // Each drawn frame is the frame as drawn without it.
    CHECK(drawn.value().pixels == draw_plain(x_of(kFrames - 1)).pixels);
    // A first frame has nothing to generate from: it shows twice.
    CHECK(generated.front().pixels == draw_plain(x_of(0)).pixels);
    // The last generated frame has the picture halfway between the last two places, under the
    // control, which the frame doesn't move.
    const render::Image& last = generated.back();
    const render::Image halfway = draw_plain(x_of(kFrames - 1) - 2.0F);
    const std::size_t off = differing(last, halfway, 40);
    CAPTURE(off);
    CHECK(off < 300);
    CHECK(off < differing(last, drawn.value(), 40) / 8);
    CHECK(off < differing(last, draw_plain(x_of(kFrames - 2)), 40) / 8);
    CHECK(red(last, 40, 4, 60, 12));

    SUBCASE("without frame generation in the settings, frames draw once") {
        canvas::Settings settings = canvas::settings(scene.world);
        settings.frame_generation = false;
        canvas::set_settings(scene.world, settings);
        generated.clear();
        REQUIRE(renderer.draw(scene.world, options).ok());
        CHECK(generated.empty());
        CHECK_FALSE(renderer.generated());
        CHECK(renderer.frames_generated() == kFrames);
    }

    SUBCASE("a device that doesn't generate frames draws them once") {
        const std::unique_ptr<render::RenderDevice> null = render::create_null_device();
        canvas::CanvasRenderer none(*null, scene.assets);
        generated.clear();
        REQUIRE(none.draw(scene.world, {.target = render::TextureId::None,
                                        .width = 64.0F,
                                        .height = 64.0F,
                                        .generate = true,
                                        .between_frames = options.between_frames})
                    .ok());
        CHECK(generated.empty());
        CHECK_FALSE(none.generated());
    }
}

TEST_CASE("project.json's rendering.frame_generation turns frame generation on") {
    json::Value project = parse(R"({"format": 1, "name": "Generated", "version": "1.0.0",
        "main_scene": "main.scene.json", "rendering": {"frame_generation": true}})");
    CHECK(slash::runtime::validate_project(project).empty());
    CHECK(slash::runtime::parse_project(project).value().frame_generation);
    project["rendering"] = json::Value(json::Object{});
    CHECK_FALSE(slash::runtime::parse_project(project).value().frame_generation);
    project["rendering"]["frame_generation"] = "yes";
    const auto errors = slash::runtime::validate_project(project);
    REQUIRE_FALSE(errors.empty());
    CHECK(errors.front().path == "/rendering/frame_generation");
}

TEST_CASE("display.set turns frame generation on and off, and display.list shows it") {
    const std::string root = "generated_frames_project";
    std::filesystem::remove_all(root);
    REQUIRE(slash::platform::create_directories(root).ok());
    REQUIRE(slash::platform::write_file(root + "/project.json", R"({"format": 1,
        "name": "Generated", "main_scene": "main.scene.json", "version": "1.0.0",
        "window": {"width": 16, "height": 8},
        "rendering": {"clear_color": [0, 0, 0, 1], "frame_generation": true}})")
                .ok());
    REQUIRE(slash::platform::write_file(root + "/main.scene.json", R"({"format": 1,
        "entities": {"bar": {"components": {"Control": {"offset_right": 4, "offset_bottom": 4},
            "ColorRect": {"color": [1, 0, 0, 1]}}}}})")
                .ok());

    auto platform = slash::platform::Platform::create({.headless = true});
    REQUIRE(platform.ok());
    auto loop =
        slash::platform::Loop::create(platform.value(), {.tick_hz = 60.0, .max_ticks = 600});
    REQUIRE(loop.ok());
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
                                                   .renderer = device.get(),
                                                   .canvas = &renderer});
        const auto ok = [&](std::string_view name, std::string_view params) {
            command::Reply reply = session.call(name, parse(params));
            REQUIRE_MESSAGE(reply.ok, reply.message);
            return std::move(reply.value);
        };
        ok("scene.open", R"({"path": "main.scene.json"})");
        CHECK(canvas::settings(session.world()).frame_generation);
        CHECK(*ok("display.list", "{}").find("frame_generation") ==
              parse(R"({"enabled": true, "in_effect": false, "frames": 0})"));

        // The game draws two frames, each after a generated one.
        const render::TextureId target = device
                                             ->create_texture({.width = 16,
                                                               .height = 8,
                                                               .filter = render::Filter::Nearest,
                                                               .render_target = true})
                                             .value();
        const canvas::CanvasOptions frame{
            .target = target, .width = 16.0F, .height = 8.0F, .generate = true};
        REQUIRE(renderer.draw(session.world(), frame).ok());
        REQUIRE(renderer.draw(session.world(), frame).ok());
        CHECK(*ok("perf.stats", "{}").find("frame_generation") ==
              parse(R"({"enabled": true, "in_effect": true, "frames": 2})"));

        // Turned off, frames draw once.
        CHECK(*ok("display.set", R"({"frame_generation": false})").find("frame_generation") ==
              parse(R"({"enabled": false, "in_effect": true, "frames": 2})"));
        REQUIRE(renderer.draw(session.world(), frame).ok());
        CHECK(*ok("display.list", "{}").find("frame_generation") ==
              parse(R"({"enabled": false, "in_effect": false, "frames": 2})"));
    }
    std::filesystem::remove_all(root);
}
