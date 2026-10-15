// Particles, as Godot's CPUParticles2D: main.scene.json has a fountain, a one-shot burst of
// sparks, smoke that was already rising when the scene opened, and a portal whose particles
// orbit in the entity's own coordinates.
//
// This program makes the dot texture, checks the particles with particles.eval as an agent
// would, with no screenshot, hears the sparks finish and sets them off again, then draws the
// scene into a texture.

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

#include <algorithm>
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

// A white dot 8 pixels across that fades to its edge. Games ship PNG files made in an image
// editor or by a generator; this one is made here so the example has no binary file.
void write_dot() {
    constexpr std::uint32_t kSize = 8;
    render::Image image{.width = kSize, .height = kSize, .pixels = {}};
    for (std::uint32_t y = 0; y < kSize; ++y) {
        for (std::uint32_t x = 0; x < kSize; ++x) {
            // Squared distance from the center, in half pixels: 2 at the middle, 98 at a corner.
            const int dx = (static_cast<int>(x) * 2) - 7;
            const int dy = (static_cast<int>(y) * 2) - 7;
            const int alpha = std::clamp(255 - ((dx * dx + dy * dy) * 4), 0, 255);
            image.pixels.insert(image.pixels.end(), {255, 255, 255, static_cast<std::uint8_t>(alpha)});
        }
    }
    check(slash::platform::write_file("dot.png", render::encode_png(image).value()));
    std::puts("wrote dot.png: 8x8");
}

// Prints the call as an agent would make it, then the reply.
json::Value call(command::Session& session, std::string_view name, std::string_view params) {
    const json::Value value = json::parse(params).value();
    std::printf("%.*s %s\n", static_cast<int>(name.size()), name.data(),
                json::write(value, json::Style::Compact).c_str());
    const command::Reply reply = session.call(name, value);
    std::puts(json::write(reply.ok ? reply.value : json::Value(reply.message), json::Style::Compact)
                  .c_str());
    return reply.value;
}

} // namespace

int main() {
    world::ComponentRegistry components;
    world::register_builtin_components(components);
    command::Registry commands;
    command::register_builtin_commands(commands);
    command::SessionOptions options;
    options.capture_logs = false;
    command::Session session(components, commands, options);

    write_dot();
    call(session, "scene.open", R"({"path": "main.scene.json"})");

    // What will the fountain look like half a second in? A time runs its settings from the
    // start, apart from the world, so nothing changes.
    call(session, "particles.eval", R"({"id": "fountain", "time": 0.5})");
    // Half a second of ticks gives the same particles: the same seed, entity ID and ticks make
    // the same particles on every platform.
    call(session, "events.subscribe", R"({"kinds": ["finished"]})");
    call(session, "step", R"({"ticks": 30})");
    call(session, "particles.eval", R"({"id": "fountain"})");

    // The sparks went off at once, in every direction, and are slowing down and fading.
    call(session, "particles.eval", R"({"id": "sparks", "list": true, "max_results": 2})");
    // The smoke ran 2 seconds of preprocess before its first tick, so it was already rising.
    // Its draw_order is "lifetime": the oldest first, so the newest puffs draw on top.
    call(session, "particles.eval", R"({"id": "smoke", "list": true, "max_results": 2})");
    // The portal's particles orbit the entity in its own coordinates; moving it moves them.
    call(session, "particles.eval", R"({"id": "portal"})");
    call(session, "component.set",
         R"({"id": "portal", "components": {"Transform2D": {"position": [180, 60]}}})");
    call(session, "particles.eval", R"({"id": "portal"})");

    // A one_shot stops emitting when its cycle ends and says "finished" when its last particle
    // dies. Setting emitting sets it off again.
    call(session, "step", R"({"ticks": 30})");
    call(session, "events.read", R"({"subscription": "events_1"})");
    call(session, "entity.get",
         R"({"id": "sparks", "components": ["CPUParticles2D"], "omit_defaults": true})");
    call(session, "component.set",
         R"({"id": "sparks", "components": {"CPUParticles2D": {"emitting": true}}})");
    call(session, "step", R"({"ticks": 1})");
    call(session, "particles.eval", R"({"id": "sparks"})");

    // An emission shape CPUParticles2D doesn't have is refused, with the ones it has.
    call(session, "component.set",
         R"({"id": "smoke", "components": {"CPUParticles2D": {"emission_shape": "sphear"}}})");

    // Draw into a 320x180 texture. Each particle is a quad of its texture (or a white pixel),
    // scaled, turned and colored; emitters next to each other in draw order with the same
    // texture and material share a GPU draw.
    const auto device =
        render::create_device_or_null({.window = nullptr, .driver = "auto", .debug = false});
    canvas::CanvasRenderer renderer(*device, session.assets());
    const render::TextureId target = device
                                         ->create_texture({.width = 320,
                                                           .height = 180,
                                                           .filter = render::Filter::Linear,
                                                           .render_target = true})
                                         .value();
    check(renderer.draw(session.world(), {.target = target,
                                          .clear = render::kDefaultClearColor,
                                          .width = 0.0F,
                                          .height = 0.0F,
                                          .interpolation = 1.0,
                                          .texture_filter = render::Filter::Linear,
                                          .time = 0.0}));
    std::printf("particles drawn: %zu, GPU draws: %llu\n", renderer.particles_drawn(),
                static_cast<unsigned long long>(device->stats().draws));
    return 0;
}
