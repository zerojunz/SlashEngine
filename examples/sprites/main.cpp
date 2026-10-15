// Sprites and flipbook animation, as in Godot: Sprite2D draws a texture, or one frame of a sprite
// sheet, and AnimatedSprite2D plays the animations in a .frames.json file on it.
//
// main.scene.json has three sprites and hero.frames.json two animations of the same sheet. This
// program writes the sheet, plays the hero's animations with the commands an agent sends, then
// draws the scene into a texture. Sessions and games set the canvas up themselves; a world of
// your own needs slash::canvas::install(world, assets).

#include "slash/canvas/canvas.hpp"
#include "slash/command/builtin.hpp"
#include "slash/command/command.hpp"
#include "slash/command/session.hpp"
#include "slash/core/json.hpp"
#include "slash/platform/file.hpp"
#include "slash/render/device.hpp"
#include "slash/render/image.hpp"
#include "slash/world/component_registry.hpp"
#include "slash/world/components.hpp"

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string_view>

namespace {

namespace canvas = slash::canvas;
namespace command = slash::command;
namespace json = slash::json;
namespace render = slash::render;
namespace world = slash::world;

// Stops with the error if a call fails, so the output never hides one.
void check(const slash::Result<void>& result) {
    if (!result) {
        std::fprintf(stderr, "%s\n", result.error().message.c_str());
        std::exit(1);
    }
}

// A sheet of four 16x16 frames in a row: red, green, blue and white. Games ship PNG files made in
// an image editor or by a generator; this one is made here so the example has no binary file.
void write_sheet(const char* path) {
    constexpr std::uint32_t kFrame = 16;
    constexpr std::array<std::array<std::uint8_t, 4>, 4> kColors{{
        {255, 0, 0, 255},
        {0, 255, 0, 255},
        {0, 0, 255, 255},
        {255, 255, 255, 255},
    }};
    render::Image sheet{.width = kFrame * 4, .height = kFrame, .pixels = {}};
    sheet.pixels.resize(std::size_t{sheet.width} * sheet.height * 4);
    for (std::uint32_t y = 0; y < sheet.height; ++y) {
        for (std::uint32_t x = 0; x < sheet.width; ++x) {
            const std::size_t at = ((std::size_t{y} * sheet.width) + x) * 4;
            for (std::size_t channel = 0; channel < 4; ++channel) {
                sheet.pixels[at + channel] = kColors.at(x / kFrame).at(channel);
            }
        }
    }
    check(slash::platform::write_file(path, render::encode_png(sheet).value()));
    std::printf("wrote %s: %ux%u, 4 frames\n", path, sheet.width, sheet.height);
}

// Prints the call as an agent would make it, then the reply.
void call(command::Session& session, std::string_view name, std::string_view params) {
    const json::Value value = json::parse(params).value();
    std::printf("%.*s %s\n", static_cast<int>(name.size()), name.data(),
                json::write(value, json::Style::Compact).c_str());
    const command::Reply reply = session.call(name, value);
    std::puts(json::write(reply.ok ? reply.value : json::Value(reply.message), json::Style::Compact)
                  .c_str());
}

} // namespace

int main() {
    write_sheet("hero.png");

    world::ComponentRegistry components;
    world::register_builtin_components(components);
    command::Registry commands;
    command::register_builtin_commands(commands);
    command::SessionOptions options;
    options.capture_logs = false;
    command::Session session(components, commands, options);

    call(session, "scene.open", R"({"path": "main.scene.json"})");
    call(session, "events.subscribe", R"({"kinds": ["animation_finished"]})");
    // "walk" plays at 8 frames a second. Ticks here are 1/16 s, so it moves a frame every two
    // ticks. The animated_sprite_2d system sets the hero's Sprite2D to the frame to draw; the
    // hero had no Sprite2D, so it gave it one.
    call(session, "step", R"({"ticks": 3, "tick_seconds": 0.0625})");
    call(session, "entity.get", R"({"id": "hero", "components": ["AnimatedSprite2D", "Sprite2D"],
        "omit_defaults": true})");
    // "jump" doesn't loop: it stops on its last frame and emits animation_finished.
    call(session, "component.set", R"({"id": "hero", "components": {"AnimatedSprite2D":
        {"animation": "jump", "frame": 0, "frame_progress": 0}}})");
    call(session, "run.until", R"({"with": ["AnimatedSprite2D"], "tick_seconds": 0.0625,
        "where": [{"component": "AnimatedSprite2D", "field": "playing", "op": "==", "value": false}],
        "include": ["AnimatedSprite2D"]})");
    call(session, "events.read", R"({"subscription": "events_1"})");

    // The two triangles the sign draws: frame 3 of the 64x16 sheet, centered on its position,
    // scaled by 2 and mirrored by flip_h (u runs from 1 back to 0.75).
    const world::World& game = session.world();
    const entt::entity sign = game.find("sign");
    const auto quad = canvas::sprite_quad(game.ecs().get<world::Sprite2D>(sign),
                                          game.ecs().get<world::Transform2D>(sign), 64, 16)
                          .value();
    std::puts("sign corners (x, y, u, v):");
    for (const std::size_t corner : std::array<std::size_t, 4>{0, 1, 4, 2}) {
        const render::Vertex2D& vertex = quad.at(corner);
        std::printf("  %g, %g, %g, %g\n", vertex.x, vertex.y, vertex.u, vertex.v);
    }

    // Between ticks, a frame draws each sprite part of the way from where it was before the last
    // tick to where it is now, so motion looks smooth at any frame rate. Rotation turns the short
    // way round: from 3 to -3 radians passes pi, not 0.
    const world::Transform2D halfway = canvas::interpolate(
        {.position = {0.0F, 0.0F}, .rotation = 3.0F, .scale = {1.0F, 1.0F}},
        {.position = {10.0F, 0.0F}, .rotation = -3.0F, .scale = {1.0F, 1.0F}}, 0.5);
    std::printf("halfway: position %g, %g, rotation %.4f\n", halfway.position.x, halfway.position.y,
                halfway.rotation);

    // Draw into a 128x96 texture. The sign has z_index -1, so it goes under the hero, and the
    // ghost is hidden. "nearest" keeps pixel art sharp for sprites that don't pick a filter.
    const auto device =
        render::create_device_or_null({.window = nullptr, .driver = "auto", .debug = false});
    canvas::CanvasRenderer renderer(*device, session.assets());
    const render::TextureId target = device
                                         ->create_texture({.width = 128,
                                                           .height = 96,
                                                           .filter = render::Filter::Linear,
                                                           .render_target = true})
                                         .value();
    check(renderer.draw(game, {.target = target,
                               .clear = render::kDefaultClearColor,
                               .width = 0.0F,
                               .height = 0.0F,
                               .interpolation = 1.0,
                               .texture_filter = render::Filter::Nearest}));
    const render::Stats stats = device->stats();
    std::printf("sprites drawn: %zu, GPU draws: %llu, triangles: %llu, textures: %u\n",
                renderer.sprites_drawn(), static_cast<unsigned long long>(stats.draws),
                static_cast<unsigned long long>(stats.triangles), stats.textures);
    return 0;
}
