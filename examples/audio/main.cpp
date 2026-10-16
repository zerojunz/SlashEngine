// Sound, as Godot's AudioServer: an AudioStreamPlayer plays music through the Music bus, an
// AudioStreamPlayer2D plays footsteps through the Sfx bus, panned and faded by where they are
// from the AudioListener2D, and both buses mix into Master. audio/default.bus_layout.json sets up
// the buses, as Godot's default_bus_layout.tres, and audio/steps.audio.json takes turns between
// two footstep sounds, as Godot's AudioStreamRandomizer.
//
// This program writes the sound files (a square wave and two clicks, made without libm so they
// are the same everywhere), then plays the game as an agent does over MCP or JSON-RPC: step runs
// ticks, audio.state shows buses and players, audio.play and audio.set_bus change what plays,
// and audio.capture measures what a bus played, or writes it to a .wav file to listen to.

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
std::vector<audio::Frame> square(int hz, float amplitude, int frames, bool fade) {
    std::vector<audio::Frame> out(static_cast<std::size_t>(frames));
    for (int i = 0; i < frames; ++i) {
        const bool high = (i * 2 * hz / kRate) % 2 == 0;
        float value = high ? amplitude : -amplitude;
        if (fade) {
            value *= 1.0F - (static_cast<float>(i) / static_cast<float>(frames));
        }
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
    // Two seconds of a 220 Hz square wave for the music, and two short fading clicks.
    write_sound("audio/theme.wav", square(220, 0.5F, 2 * kRate, false));
    write_sound("audio/step_left.wav", square(880, 0.8F, kRate / 10, true));
    write_sound("audio/step_right.wav", square(660, 0.8F, kRate / 10, true));
    std::puts("wrote audio/theme.wav, audio/step_left.wav and audio/step_right.wav");

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
    call(session, "events.subscribe", R"({"kinds": ["finished"]})");
    // The music has autoplay, so it starts at the first tick. Each tick of 1/60 s mixes 735
    // frames at 44100 Hz.
    call(session, "step", R"({"ticks": 6})");
    // Music's -6 dB halves the square wave's 0.5 to a peak of -12 dB. Its low pass filter is
    // off, so only enabled: false shows. Sfx is silent: the footsteps haven't played.
    call(session, "audio.state", "{}");

    // The footsteps are 200 pixels left of the listener, out of max_distance 2000: they fade to
    // 0.9 and pan left by 200 / 640 of the window, at half the project's panning strength.
    call(session, "audio.play", R"({"id": "footsteps"})");
    call(session, "step", R"({"ticks": 3})");
    call(session, "audio.state", R"({"ids": ["footsteps"]})");
    // The Sfx bus over the last twentieth of a second: the click fading, slice by slice.
    call(session, "audio.capture", R"({"bus": "Sfx", "seconds": 0.05, "slices": 5})");

    // The click ends and says so. The next play takes the other click, its turn in the
    // randomizer, panned right now that the footsteps are right of the listener.
    call(session, "step", R"({"ticks": 6})");
    call(session, "events.read", R"({"subscription": "events_1"})");
    call(session, "component.set",
         R"({"id": "footsteps", "components": {"Transform2D": {"position": [520, 180]}}})");
    call(session, "audio.play", R"({"id": "footsteps"})");
    call(session, "step", R"({"ticks": 1})");
    call(session, "audio.state", R"({"ids": ["footsteps"]})");

    // Mute the music, as an options menu would; the bus layout file is unchanged.
    call(session, "audio.set_bus", R"({"bus": "Music", "mute": true})");
    call(session, "step", R"({"ticks": 2})");
    call(session, "audio.capture", R"({"seconds": 0.01, "slices": 0})");
    call(session, "audio.set_bus", R"({"bus": "Music", "mute": false})");
    call(session, "step", R"({"ticks": 30})");

    // Half a second of Master as a .wav file, to listen to.
    call(session, "audio.capture", R"({"seconds": 0.5, "path": "build/master.wav"})");
    return 0;
}
