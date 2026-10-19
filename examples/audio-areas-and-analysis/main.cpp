// Areas that change a sound's bus, and effects that measure and keep what plays, as Godot's
// Area2D audio bus override, AudioEffectPitchShift, AudioEffectSpectrumAnalyzer,
// AudioEffectCapture and AudioEffectRecord. audio/default.bus_layout.json puts a spectrum
// analyzer, a capture and a record effect on Master, a pitch shift an octave up on Voice and a
// reverb on Cave; the cave's Area2D sends 2D sounds inside it to Cave.
//
// This program writes the sound files (square waves, made without libm so they are the same
// everywhere), then plays the game as an agent does over MCP or JSON-RPC: audio.spectrum reads
// the analyzer, audio.state shows the bus each player plays through, and audio.record records
// Master and writes the recording to a .wav file.

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
    // A 220 Hz hum for the voice and an 880 Hz chirp for the bat, three seconds each.
    write_sound("audio/hum.wav", square(220, 0.5F, 3 * kRate));
    write_sound("audio/chirp.wav", square(880, 0.25F, 3 * kRate));
    std::puts("wrote audio/hum.wav and audio/chirp.wav");

    world::ComponentRegistry components;
    world::register_builtin_components(components);
    command::Registry commands;
    command::register_builtin_commands(commands);
    command::SessionOptions options;
    options.project_root = ".";
    options.capture_logs = false;
    command::Session session(components, commands, options);

    call(session, "scene.open", R"({"path": "main.scene.json"})");
    // The hum autoplays on Voice, whose pitch shift takes it up an octave: Master's spectrum
    // analyzer hears 440 Hz, in the 355 to 710 Hz band, and the hum's own 220 Hz band is far
    // quieter. The square wave's odd harmonics fill the bands above.
    call(session, "step", R"({"ticks": 12})");
    call(session, "audio.spectrum", "{}");
    // With the pitch shift off, the hum is back at 220 Hz.
    call(session, "audio.set_bus", R"({"bus": "Voice", "effect": 0, "enabled": false})");
    call(session, "step", R"({"ticks": 6})");
    call(session, "audio.spectrum", R"({"ranges": [[200, 240], [420, 460]]})");
    call(session, "audio.set_bus", R"({"bus": "Voice", "mute": true})");

    // The bat chirps outside the cave, on its own bus, Master...
    call(session, "audio.play", R"({"id": "bat"})");
    call(session, "step", R"({"ticks": 2})");
    call(session, "audio.state", R"({"ids": ["bat"]})");
    // ...and flies in: the cave's Area2D overrides its bus with Cave, and its reverb.
    call(session, "component.set",
         R"({"id": "bat", "components": {"Transform2D": {"position": [480, 180]}}})");
    call(session, "step", R"({"ticks": 2})");
    call(session, "audio.state", R"({"ids": ["bat"]})");

    // Half a second of Master, recorded and written to a .wav file to listen to.
    call(session, "audio.record", R"({"recording": true})");
    call(session, "step", R"({"ticks": 30})");
    call(session, "audio.record", R"({"recording": false, "path": "build/take.wav"})");
    // The record effect keeps its recording. The capture effect has held all it can: its ring
    // of 8192 frames (0.1 s rounded up to a power of two) holds 8191, and each mix step of 512
    // frames goes in whole, so 15 steps. It discards the rest until game code takes some with
    // get_buffer.
    call(session, "audio.state", "{}");
    return 0;
}
