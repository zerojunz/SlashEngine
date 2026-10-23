// 7.1 surround sound, as Godot's speaker modes: project.json's audio.speaker_mode mixes for eight
// speakers. A bee flies round the listener and Godot's Spcap panning moves its buzz from speaker
// to speaker; music plays on every speaker (mix_target "surround") and an announcer on the
// center speaker alone (mix_target "center").
//
// This program writes the sound files (square waves, made without libm so they are the same
// everywhere), then plays the game as an agent does over MCP or JSON-RPC: step runs ticks,
// audio.state shows each player's gain on each speaker, and audio.capture measures each
// speaker's level and writes a 7.1 .wav file.

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
    // Five seconds each of a buzz, a low drone of music and a higher voice.
    write_sound("buzz.wav", square(220, 0.25F, 5 * kRate));
    write_sound("music.wav", square(110, 0.5F, 5 * kRate));
    write_sound("voice.wav", square(440, 0.5F, 5 * kRate));
    std::puts("wrote buzz.wav, music.wav and voice.wav");

    world::ComponentRegistry components;
    world::register_builtin_components(components);
    command::Registry commands;
    command::register_builtin_commands(commands);
    command::SessionOptions options;
    options.project_root = ".";
    options.capture_logs = false;
    // The session reads project.json's speaker mode.
    command::Session session(components, commands, options);

    call(session, "scene.open", R"({"path": "main.scene.json"})");
    // One second in, the bee is 10 meters to the right, at its unit_size: its buzz is on the
    // right speakers, loudest on the side right one, and nothing comes from the side left one,
    // which faces away. Its LFE gets all of it, as Godot's does.
    call(session, "step", R"({"ticks": 60})");
    call(session, "audio.state", R"({"ids": ["bee"]})");
    // Each speaker's level over the last tenth of a second: the music on all of them, the
    // announcer on the center, and the bee on the right.
    call(session, "audio.capture", R"({"seconds": 0.1, "slices": 0})");
    // At two seconds it is behind: the rear speakers carry it.
    call(session, "step", R"({"ticks": 60})");
    call(session, "audio.state", R"({"ids": ["bee"]})");
    // The music and the announcer don't move: their mix targets pick their speakers.
    call(session, "audio.state", R"({"ids": ["music", "announcer"]})");
    // A .wav file of the eight channels, which names its speakers.
    call(session, "audio.capture", R"({"seconds": 0.5, "slices": 0, "path": "surround.wav"})");
    return 0;
}
