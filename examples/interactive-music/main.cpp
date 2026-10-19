// Music that follows the game, as Godot's AudioStreamInteractive, AudioStreamSynchronized and
// AudioStreamPlaylist. audio/score.audio.json has three clips: explore and combat loop for eight
// beats at 120 BPM, and a victory sting goes back to explore at its end. Its transitions say
// when each switch happens: on combat's next bar with a crossfade, on the next beat to victory.
// audio/layers.audio.json plays drums and a melody together; audio/radio.audio.json plays a
// song, then the news, then starts over.
//
// This program writes the sound files (square waves, made without libm so they are the same
// everywhere), then plays the game as an agent does over MCP or JSON-RPC: component.set changes
// a player's parameters (switch_to_clip, stream_1_volume_db), audio.state shows the clip playing
// and each bus's peak, and entity.get shows switch_to_clip following the music.

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
    // Each clip a different note and loudness, so audio.state's peaks tell them apart.
    write_sound("audio/explore.wav", square(220, 0.25F, 4 * kRate)); // eight beats at 120 BPM
    write_sound("audio/combat.wav", square(330, 0.5F, 4 * kRate));
    write_sound("audio/victory.wav", square(440, 0.125F, kRate));
    write_sound("audio/drums.wav", square(110, 0.25F, 4 * kRate));
    write_sound("audio/melody.wav", square(440, 0.25F, 4 * kRate));
    write_sound("audio/song.wav", square(262, 0.5F, kRate));
    write_sound("audio/news.wav", square(196, 0.125F, kRate));
    std::puts("wrote the sounds in audio/");

    world::ComponentRegistry components;
    world::register_builtin_components(components);
    command::Registry commands;
    command::register_builtin_commands(commands);
    command::SessionOptions options;
    options.project_root = ".";
    options.capture_logs = false;
    command::Session session(components, commands, options);

    call(session, "scene.open", R"({"path": "main.scene.json"})");
    // The score autoplays its first clip, explore, at -12 dB.
    call(session, "step", R"({"ticks": 30})");
    call(session, "audio.state", R"({"ids": ["score"]})");

    // The fight starts. Explore to combat waits for explore's next bar, two seconds in...
    call(
        session, "component.set",
        R"({"id": "score", "components": {"AudioStreamPlayer": {"parameters": {"switch_to_clip": "combat"}}}})");
    call(session, "step", R"({"ticks": 30})");
    call(session, "audio.state", R"({"ids": ["score"]})");
    // ...then crossfades over a beat, and combat plays at -6 dB.
    call(session, "step", R"({"ticks": 90})");
    call(session, "audio.state", R"({"ids": ["score"]})");

    // The fight is won. The victory sting starts on combat's next beat as combat fades out; the
    // sting goes back to explore at its end, so switch_to_clip already names explore.
    call(
        session, "component.set",
        R"({"id": "score", "components": {"AudioStreamPlayer": {"parameters": {"switch_to_clip": "victory"}}}})");
    call(session, "step", R"({"ticks": 45})");
    call(session, "audio.state", R"({"ids": ["score"]})");
    call(session, "entity.get", R"({"id": "score", "components": ["AudioStreamPlayer"]})");
    // A second later explore fades in over half a second, as the transition to it says.
    call(session, "step", R"({"ticks": 75})");
    call(session, "audio.state", R"({"ids": ["score"]})");
    call(session, "audio.stop", R"({"id": "score"})");

    // The layers start together; the melody's parameter keeps it silent, so the drums play alone
    // at -12 dB. Turning the melody up adds its 0.25 to theirs: -6 dB.
    call(session, "audio.play", R"({"id": "layers"})");
    call(session, "step", R"({"ticks": 10})");
    call(session, "audio.state", R"({"ids": ["layers"]})");
    call(
        session, "component.set",
        R"({"id": "layers", "components": {"AudioStreamPlayer": {"parameters": {"stream_1_volume_db": 0}}}})");
    call(session, "step", R"({"ticks": 10})");
    call(session, "audio.state", R"({"ids": ["layers"]})");
    call(session, "audio.stop", R"({"id": "layers"})");

    // The radio plays the song (-6 dB) for its second, then the news (-18 dB) as the song fades
    // out under it over a quarter of a second.
    call(session, "audio.play", R"({"id": "radio"})");
    call(session, "step", R"({"ticks": 30})");
    call(session, "audio.state", R"({"ids": ["radio"]})");
    call(session, "step", R"({"ticks": 60})");
    call(session, "audio.state", R"({"ids": ["radio"]})");
    return 0;
}
