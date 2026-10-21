// GPU particles, as Godot's GPUParticles2D: main.scene.json has a fire, a one-shot burst of sparks
// that bursts the same way each time, snow that was already falling when the scene opened, smoke
// that curls in turbulence, rain that lands on a ledge, fireworks, rockets that burst into
// sparks in a sub-emitter, and comets that draw trails.
// How each one's particles start and move is a .process_material.json file, Godot's
// ParticleProcessMaterial in 2D.
//
// This program makes the dot and streak textures, checks the particles with particles.eval as an
// agent would, with no screenshot, hears the sparks finish and sets them off again, then draws the
// scene, where the device runs the particles.

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
            const int alpha = std::clamp(255 - (((dx * dx) + (dy * dy)) * 4), 0, 255);
            image.pixels.insert(image.pixels.end(),
                                {255, 255, 255, static_cast<std::uint8_t>(alpha)});
        }
    }
    check(slash::platform::write_file("dot.png", render::encode_png(image).value()));
    std::puts("wrote dot.png: 8x8");
}

// A streak for the comets' trails, 8 pixels across and 32 down, that a trail ribbon stretches
// from its particle down its length: bright in the middle of its width, fading toward its end.
void write_streak() {
    constexpr std::uint32_t kWidth = 8;
    constexpr std::uint32_t kHeight = 32;
    render::Image image{.width = kWidth, .height = kHeight, .pixels = {}};
    for (std::uint32_t y = 0; y < kHeight; ++y) {
        for (std::uint32_t x = 0; x < kWidth; ++x) {
            const int dx = (static_cast<int>(x) * 2) - 7; // -7 to 7
            const int across = 255 - (dx * dx * 5);
            const int alpha = std::clamp(
                across * static_cast<int>(kHeight - y) / static_cast<int>(kHeight), 0, 255);
            image.pixels.insert(image.pixels.end(),
                                {255, 255, 255, static_cast<std::uint8_t>(alpha)});
        }
    }
    check(slash::platform::write_file("streak.png", render::encode_png(image).value()));
    std::puts("wrote streak.png: 8x32");
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
    write_streak();
    call(session, "scene.open", R"({"path": "main.scene.json"})");

    // What will the fire look like half a second in? A time runs its settings from the start, on
    // the CPU and apart from the world, so nothing changes.
    call(session, "particles.eval", R"({"id": "fire", "time": 0.5})");
    // Half a second of ticks gives the same particles: each tick the gpu_particles_2d system
    // works out the emitter's steps (30 a second, its fixed_fps), and the same steps make the
    // same particles on every device, on the CPU here as on a GPU.
    call(session, "events.subscribe", R"({"kinds": ["finished"]})");
    call(session, "step", R"({"ticks": 30})");
    call(session, "particles.eval", R"({"id": "fire"})");

    // The sparks went off at once, in every direction, and are slowing down and fading.
    call(session, "particles.eval", R"({"id": "sparks", "list": true, "max_results": 2})");
    // The snow ran 6 seconds of preprocess before its first tick, so it was already falling.
    call(session, "particles.eval", R"({"id": "snow"})");
    // The smoke curls as it rises: turbulence turns each particle's velocity along curl noise
    // that drifts up, by its turbulence_influence, as Godot's.
    call(session, "particles.eval", R"({"id": "smoke", "list": true, "max_results": 2})");

    // A one_shot stops emitting when its cycle ends and says "finished" once its particles have
    // had time to die, as Godot's: lifetime times 2 - explosiveness after it started. Setting
    // emitting sets it off again; with use_fixed_seed, the burst is the same as the first.
    call(session, "step", R"({"ticks": 30})");
    call(session, "events.read", R"({"subscription": "events_1"})");
    call(session, "entity.get",
         R"({"id": "sparks", "components": ["GPUParticles2D"], "omit_defaults": true})");
    call(session, "component.set",
         R"({"id": "sparks", "components": {"GPUParticles2D": {"emitting": true}}})");
    call(session, "step", R"({"ticks": 30})");
    call(session, "particles.eval", R"({"id": "sparks", "list": true, "max_results": 2})");

    // The rain has landed on the ledge, a LightOccluder2D 40 pixels wide whose top is at y 100:
    // its process material's collision_mode "rigid" stops particles at LightOccluder2D outlines,
    // as Godot's, so the oldest rest on it, half the dot's average side (4 pixels) above it.
    call(session, "particles.eval", R"({"id": "rain", "list": true, "max_results": 3})");

    // Fireworks: the rockets' sub_emitter is the burst, and their process material's
    // sub_emitter_mode "at_end" starts 16 of its particles where each rocket dies of age, as
    // Godot's. The burst emits nothing on its own and steps with the rockets; three rockets have
    // burst so far, 16 sparks each.
    call(session, "particles.eval", R"({"id": "rockets"})");
    call(session, "particles.eval", R"({"id": "burst"})");

    // Comets: with trail_enabled each draws as Godot's trail ribbon, the streak stretched down
    // the path it took over the last trail_lifetime (0.4 s), through trail_sections (12) copies
    // of it that follow that path behind it. Their process material's particle_flag_align_y
    // turns each along its velocity, so the ribbon is as wide as the streak across the path.
    // particles.eval gives the trails' bounds and, for each comet, how far its trail reaches.
    call(session, "particles.eval", R"({"id": "comets", "list": true, "max_results": 1})");

    // A process material with a mistake is refused with the JSON Pointer of the bad value.
    // "slash-cli scene validate" finds the same mistake in the file before anything runs it, and
    // exports check every process material.
    check(slash::platform::write_file("broken.process_material.json",
                                      R"({"format": 1, "emission_shape": "sphear"})"));
    call(session, "component.set",
         R"({"id": "fire", "components": {"GPUParticles2D": {"process_material":
             "broken.process_material.json"}}})");
    call(session, "particles.eval", R"({"id": "fire"})");
    call(session, "undo", "{}");

    // Is the fire drawing? frame.capture draws the frame on the CPU, the same on every machine,
    // and returns the colors of the pixels asked for: the fire's middle, and the sky beside it.
    call(session, "frame.capture", R"({"points": [[80, 140], [130, 140]]})");

    // Draw into a 320x180 texture. The device runs each emitter's steps it hasn't run, then
    // draws its particles as quads of its texture, scaled, turned and colored, in one draw each.
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
