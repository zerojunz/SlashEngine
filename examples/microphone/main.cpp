// The microphone, as Godot's AudioStreamMicrophone and AudioServer input calls. project.json turns
// on audio.enable_input, and the "voice" player plays audio/mic.audio.json on the muted "Mic" bus,
// as in Godot's microphone demo, so its effects hear the player without the speakers playing it.
// A gameplay system in C++ reads the same input with Server::input_frames and lights a lantern
// for each half second of singing.
//
// Headless there is no microphone, so the input is silent until audio.input plays a sound into it
// in place of the microphone, as an agent tests a game that listens. This program writes that
// sound (made without libm, so it is the same everywhere) and adds the lantern system, then plays
// the game as an agent does over MCP or JSON-RPC.

#include "slash/audio/audio.hpp"
#include "slash/command/builtin.hpp"
#include "slash/command/command.hpp"
#include "slash/command/session.hpp"
#include "slash/core/json.hpp"
#include "slash/platform/file.hpp"
#include "slash/world/component_registry.hpp"
#include "slash/world/components.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

namespace audio = slash::audio;
namespace command = slash::command;
namespace json = slash::json;
namespace world = slash::world;

constexpr int kRate = 44100;

// Someone singing a 330 Hz square wave: quietly for 0.4 seconds, loudly for 1.2, quietly for 0.4.
constexpr std::array<std::pair<int, float>, 3> kParts{
    {{kRate * 2 / 5, 0.05F}, {kRate * 6 / 5, 0.4F}, {kRate * 2 / 5, 0.05F}}};

std::vector<audio::Frame> song() {
    std::vector<audio::Frame> out;
    for (const auto& [frames, amplitude] : kParts) {
        for (int i = 0; i < frames; ++i) {
            const float value = (i * 2 * 330 / kRate) % 2 == 0 ? amplitude : -amplitude;
            out.push_back({value, value});
        }
    }
    return out;
}

// Lights a lantern for each 30 ticks in a row of loud input, as a game reads Godot's
// AudioServer.get_input_frames each frame. Input records while the voice player keeps it on.
void lanterns(world::World& w, int& loud_ticks, int& lit) {
    audio::Server* server = audio::server(w);
    const std::vector<audio::Frame> frames = server->input_frames(server->input_frames_available());
    float peak = 0.0F;
    for (const audio::Frame& frame : frames) {
        peak = std::max({peak, std::abs(frame.left), std::abs(frame.right)});
    }
    loud_ticks = peak >= 0.25F ? loud_ticks + 1 : 0;
    if (loud_ticks > 0 && loud_ticks % 30 == 0) {
        json::Value data;
        data["lantern"] = ++lit;
        w.emit("lantern_lit", "voice", std::move(data));
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
    if (!slash::platform::write_file("audio/song.wav", audio::encode_wav(song(), kRate))) {
        std::puts("could not write audio/song.wav");
    }
    std::puts("wrote audio/song.wav");

    world::ComponentRegistry components;
    world::register_builtin_components(components);
    command::Registry commands;
    command::register_builtin_commands(commands);
    command::SessionOptions options;
    options.project_root = ".";
    options.capture_logs = false;
    command::Session session(components, commands, options);
    int loud_ticks = 0;
    int lit = 0;
    session.world().add_system(
        "lanterns", [&](world::World& w, double /*seconds*/) { lanterns(w, loud_ticks, lit); });

    call(session, "scene.open", R"({"path": "main.scene.json"})");
    call(session, "events.subscribe", R"({"kinds": ["lantern_lit"]})");
    // The voice autoplays, which turns input on. With no microphone, it records silence.
    call(session, "step", R"({"ticks": 5})");
    call(session, "audio.state", R"({"ids": ["voice"]})");

    // The song plays into the input from the next tick, and the Mic bus records what the voice
    // plays, 50 ms behind.
    call(session, "audio.input", R"({"stream": "audio/song.wav"})");
    call(session, "audio.record", R"({"bus": "Mic", "recording": true})");
    // A second in, the singing is loud: the Mic bus's spectrum analyzer hears 330 Hz and its
    // third harmonic, but not 2 kHz, and Master is silent because Mic is muted.
    call(session, "step", R"({"ticks": 60})");
    call(session, "audio.spectrum", R"({"bus": "Mic", "ranges": [[300, 360], [960, 1020],
                                                                 [1900, 2100]]})");
    call(session, "audio.capture", R"({"bus": "Master", "seconds": 1, "slices": 0})");
    // In the quiet end, the input's peak is the song's 0.05: -26 dB.
    call(session, "step", R"({"ticks": 50})");
    call(session, "audio.state", R"({"ids": ["voice"]})");
    // 1.2 seconds of loud singing lit two lanterns, one per half second.
    call(session, "events.read", R"({"subscription": "events_1"})");
    call(session, "audio.record", R"({"bus": "Mic", "recording": false})");
    return 0;
}
