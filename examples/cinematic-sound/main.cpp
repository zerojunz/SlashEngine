// A cutscene's sound, as Godot's audio playback tracks: intro.anim.json's audio tracks start
// sounds on the door's and the narrator's AudioStreamPlayers at their keys. Each sound plays on a
// voice of the player's own, beside whatever the player plays itself, so a cutscene can speak
// over a player's music.
//
// This program writes the sounds (square waves made without libm, so they are the same
// everywhere), then plays the scene as an agent does over MCP or JSON-RPC: step runs ticks,
// audio.state shows how many of each player's sounds came from tracks, audio.capture measures
// what played, and animation.seek jumps back for a second take of a line.

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
    // Half a second of a low creak, a second and a half of the line and a sigh of 0.8 seconds.
    write_sound("creak.wav", square(110, 0.5F, kRate / 2));
    write_sound("line.wav", square(330, 0.25F, 3 * kRate / 2));
    write_sound("sigh.wav", square(220, 0.25F, 4 * kRate / 5));
    std::puts("wrote creak.wav, line.wav and sigh.wav");

    world::ComponentRegistry components;
    world::register_builtin_components(components);
    command::Registry commands;
    command::register_builtin_commands(commands);
    command::SessionOptions options;
    options.project_root = ".";
    options.capture_logs = false;
    command::Session session(components, commands, options);

    call(session, "scene.open", R"({"path": "main.scene.json"})");
    call(session, "events.subscribe", R"({"kinds": ["animation_finished"]})");

    // The creak's key is at a quarter of a second, the 15th tick. It plays on the door's player,
    // whose own stream is empty, at the player's -6 dB: a peak of -12 dB.
    call(session, "step", R"({"ticks": 20})");
    call(session, "audio.state", R"({"ids": ["door", "narrator"]})");
    call(session, "audio.capture", R"({"seconds": 0.05, "slices": 0})");

    // The line starts at one second, a quarter of a second into its sound (start_offset), and
    // stops half a second before the sound's end (end_offset): it plays for 0.75 seconds.
    call(session, "step", R"({"ticks": 50})");
    call(session, "audio.state", R"({"ids": ["narrator"]})");
    call(session, "step", R"({"ticks": 40})");
    call(session, "audio.state", R"({"ids": ["narrator"]})");

    // A jump starts no sound, as Godot's update-only seek. Jumping back to the "retake" marker,
    // just before the line's key, plays the line again once playback passes the key.
    call(session, "animation.seek", R"({"id": "director", "marker": "retake"})");
    call(session, "audio.state", R"({"ids": ["narrator"]})");
    call(session, "step", R"({"ticks": 10})");
    call(session, "audio.state", R"({"ids": ["narrator"]})");

    // The sigh plays at 2.5 seconds, and the animation's end at 3 seconds stops it, 0.3 seconds
    // before the sound's own end, as Godot stops a finished animation's sounds.
    call(session, "step", R"({"ticks": 95})");
    call(session, "audio.state", R"({"ids": ["narrator"]})");
    call(session, "step", R"({"ticks": 30})");
    call(session, "audio.state", R"({"ids": ["narrator"]})");
    call(session, "events.read", R"({"subscription": "events_1"})");
    return 0;
}
