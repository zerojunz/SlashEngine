// 3D sound, as Godot's AudioStreamPlayer3D: a car drives past a listener along a road 10 meters
// ahead, its keyframe animation moving it at 20 meters a second, so its engine fades in and
// out, pans from left to right and drops in pitch as it passes, by the Doppler effect. A stream
// trickles inside a cave, an Area3D that sends the sounds in it to its Cave bus and a share of
// them to its Echo bus, which has a reverb.
//
// This program writes the sound files (square waves, made without libm so they are the same
// everywhere), then plays the game as an agent does over MCP or JSON-RPC: step runs ticks,
// audio.state shows each player's volume, pitch and filter, and audio.capture measures what the
// Echo bus played.

#include "slash/audio/audio.hpp"
#include "slash/command/builtin.hpp"
#include "slash/command/command.hpp"
#include "slash/command/session.hpp"
#include "slash/core/json.hpp"
#include "slash/platform/file.hpp"
#include "slash/world/component_registry.hpp"
#include "slash/world/components.hpp"

#include <cstdio>
#include <string>
#include <string_view>
#include <vector>

namespace {

namespace audio = slash::audio;
namespace command = slash::command;
namespace json = slash::json;
namespace world = slash::world;

constexpr int kRate = 44100;

// A square wave: +amplitude for half of each cycle, -amplitude for the other half.
std::vector<audio::Frame> square(int hz, float amplitude, int frames) {
    std::vector<audio::Frame> out(static_cast<std::size_t>(frames));
    for (int i = 0; i < frames; ++i) {
        const float value = (i * 2 * hz / kRate) % 2 == 0 ? amplitude : -amplitude;
        out[static_cast<std::size_t>(i)] = {value, value};
    }
    return out;
}

void write_sound(const std::string& path, const std::vector<audio::Frame>& frames) {
    if (!slash::platform::write_file(path, audio::encode_wav(frames, kRate))) {
        std::printf("could not write %s\n", path.c_str());
    }
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
    // Five seconds of a low engine and of a higher trickle.
    write_sound("audio/engine.wav", square(110, 0.5F, 5 * kRate));
    write_sound("audio/trickle.wav", square(330, 0.25F, 5 * kRate));
    std::puts("wrote audio/engine.wav and audio/trickle.wav");

    world::ComponentRegistry components;
    world::register_builtin_components(components);
    command::Registry commands;
    command::register_builtin_commands(commands);
    command::SessionOptions options;
    options.project_root = ".";
    options.capture_logs = false;
    // The session reads project.json's bus layout.
    command::Session session(components, commands, options);

    call(session, "scene.open", R"({"path": "main.scene.json"})");
    // Half a second in, the car is 30 meters left of the road's middle and coming: louder on the
    // left, faded by inverse_distance from its unit_size of 5 meters and by max_distance, its
    // highs muffled for that fade, and higher by the Doppler effect.
    call(session, "step", R"({"ticks": 30})");
    call(session, "audio.state", R"({"ids": ["car"]})");
    // At two seconds it passes straight ahead, 10 meters off: the same on both sides, at its own
    // pitch.
    call(session, "step", R"({"ticks": 90})");
    call(session, "audio.state", R"({"ids": ["car"]})");
    // At three seconds it is 20 meters right and going: louder on the right and lower.
    call(session, "step", R"({"ticks": 60})");
    call(session, "audio.state", R"({"ids": ["car"]})");

    // The trickle is in the cave: it plays on the cave's bus, and sends 0.6 of itself to Echo,
    // whose reverb plays it back.
    call(session, "audio.state", R"({"ids": ["trickle"]})");
    call(session, "audio.capture", R"({"bus": "Echo", "seconds": 0.5, "slices": 5})");
    return 0;
}
