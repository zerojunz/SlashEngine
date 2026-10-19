// Sound a game makes as it runs, as Godot's AudioStreamGenerator and AudioStreamPolyphonic.
// audio/synth.audio.json is a generator: a gameplay system in C++ pushes a square-wave tune into
// it each tick, through audio::stream_playback. audio/sfx.audio.json is polyphonic: one player
// plays many sounds at once, each started by audio.play's "stream" at its own volume and pitch.
//
// This program writes the hit sound (made without libm, so it is the same everywhere) and adds
// the synth system, then plays the game as an agent does over MCP or JSON-RPC: audio.state shows
// the generator's free frames and skips (mixes that ran out of frames) and the streams the
// polyphonic player has going, and audio.capture shows the tune and the hits slice by slice.

#include "slash/audio/audio.hpp"
#include "slash/command/builtin.hpp"
#include "slash/command/command.hpp"
#include "slash/command/session.hpp"
#include "slash/core/json.hpp"
#include "slash/platform/file.hpp"
#include "slash/world/component_registry.hpp"
#include "slash/world/components.hpp"

#include <array>
#include <cstdint>
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
constexpr int kSynthRate = 22050; // audio/synth.audio.json's mix_rate

// A square wave fading out: +amplitude for half of each cycle, -amplitude for the other half.
std::vector<audio::Frame> click(int hz, float amplitude, int frames) {
    std::vector<audio::Frame> out(static_cast<std::size_t>(frames));
    for (int i = 0; i < frames; ++i) {
        float value = (i * 2 * hz / kRate) % 2 == 0 ? amplitude : -amplitude;
        value *= 1.0F - (static_cast<float>(i) / static_cast<float>(frames));
        out[static_cast<std::size_t>(i)] = {value, value};
    }
    return out;
}

// The tune: four notes of a quarter of a second each, then nothing more.
constexpr std::array<int, 4> kNotes{220, 262, 330, 440};
constexpr std::int64_t kNoteFrames = kSynthRate / 4;

// Fills the synth's generator with the tune's next frames, as Godot's AudioStreamGenerator demo
// fills its playback in _process. The playback is there from the tick after the player starts.
void push_tune(world::World& w, std::int64_t& pushed) {
    audio::Playback* playback = audio::stream_playback(w, w.find("synth"));
    audio::GeneratorPlayback* generator = playback != nullptr ? playback->generator() : nullptr;
    if (generator == nullptr) {
        return;
    }
    std::vector<audio::Frame> frames;
    const std::int64_t end = kNoteFrames * static_cast<std::int64_t>(kNotes.size());
    for (int room = generator->frames_available(); room > 0 && pushed < end; --room, ++pushed) {
        const std::int64_t hz = kNotes[static_cast<std::size_t>(pushed / kNoteFrames)];
        const float value = (pushed * 2 * hz / kSynthRate) % 2 == 0 ? 0.25F : -0.25F;
        frames.push_back({value, value});
    }
    generator->push_buffer(frames);
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
    if (!slash::platform::write_file("audio/hit.wav",
                                     audio::encode_wav(click(880, 0.5F, kRate / 10), kRate))) {
        std::puts("could not write audio/hit.wav");
    }
    std::puts("wrote audio/hit.wav");

    world::ComponentRegistry components;
    world::register_builtin_components(components);
    command::Registry commands;
    command::register_builtin_commands(commands);
    command::SessionOptions options;
    options.project_root = ".";
    options.capture_logs = false;
    command::Session session(components, commands, options);
    // Gameplay systems run before the audio system, so each tick's frames are pushed before
    // they are mixed.
    std::int64_t pushed = 0;
    session.world().add_system(
        "synth", [&pushed](world::World& w, double /*seconds*/) { push_tune(w, pushed); });

    call(session, "scene.open", R"({"path": "main.scene.json"})");
    // The synth autoplays. Its first tick has no frames yet (the system runs before the voice
    // starts), so those mixes count as skips; from then on the system keeps it full. Its
    // buffer holds 4095 frames, 0.1 seconds at 22050 Hz rounded up to a power of two, as Godot's.
    // Master peaks at -11 dB: the square's 0.25 is -12 dB, and resampling it to 44100 Hz
    // overshoots its edges a little.
    call(session, "step", R"({"ticks": 20})");
    call(session, "audio.state", R"({"ids": ["synth"]})");

    // Two hits on one player: the first at -6 dB, the second an octave up (pitch scale 2), so
    // it is over in half the time.
    call(session, "audio.play", R"({"id": "sfx", "stream": "audio/hit.wav", "volume_db": -6})");
    call(session, "audio.play", R"({"id": "sfx", "stream": "audio/hit.wav", "pitch_scale": 2})");
    call(session, "step", R"({"ticks": 2})");
    call(session, "audio.state", R"({"ids": ["sfx"]})");
    call(session, "step", R"({"ticks": 4})");
    call(session, "audio.state", R"({"ids": ["sfx"]})");

    // The tune is pushed in a second's worth of frames, then the system stops: the generator
    // empties, plays silence and counts a skip for each mix that runs out.
    call(session, "step", R"({"ticks": 50})");
    call(session, "audio.state", R"({"ids": ["synth", "sfx"]})");
    // Master over the last 1.2 seconds, in tenths: the tune, louder where the hits played over
    // it, then silence.
    call(session, "audio.capture", R"({"seconds": 1.2, "slices": 12})");
    return 0;
}
