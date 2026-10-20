// Judging music without listening: audio.analyze measures a sound's loudness, peaks, clipping,
// silence, tempo, key, frequency balance and how cleanly its loop joins.
//
// This program writes a four-bar groove in D minor at 100 beats per minute (kick, hi-hat, bass
// and chords, made without libm so the sound is the same everywhere), two .audio.json files
// that loop it, one well and one cut short, and a copy turned up until it clips. Then it
// measures each as an agent does over MCP or JSON-RPC, and what a bus played while the groove
// looped.

#include "slash/audio/audio.hpp"
#include "slash/command/builtin.hpp"
#include "slash/command/command.hpp"
#include "slash/command/session.hpp"
#include "slash/core/json.hpp"
#include "slash/core/math.hpp"
#include "slash/platform/file.hpp"
#include "slash/world/component_registry.hpp"
#include "slash/world/components.hpp"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <string>
#include <string_view>
#include <vector>

namespace {

namespace audio = slash::audio;
namespace command = slash::command;
namespace json = slash::json;

constexpr int kRate = 44100;
constexpr double kBeat = 0.6; // seconds: 100 beats per minute
constexpr int kBeats = 16;    // four bars of four
constexpr double kTwoPi = 6.283185307179586;

// A MIDI note's frequency: 440 Hz for A4 (69), a twelfth root of two per semitone.
double note_hz(int midi) {
    constexpr double kSemitone = 1.0594630943592953;
    double hz = 440.0;
    for (int n = 69; n < midi; ++n) {
        hz *= kSemitone;
    }
    for (int n = 69; n > midi; --n) {
        hz /= kSemitone;
    }
    return hz;
}

// Adds a note from start for length seconds: a sine rising over 5 ms, then dying away to
// nothing at its end (a pluck) or holding until its last 20 ms (a pad).
void play(std::vector<audio::Frame>& out, int midi, double start, double length, float volume,
          bool pluck) {
    const double hz = note_hz(midi);
    const auto first = static_cast<std::size_t>(start * kRate);
    const auto count = static_cast<std::size_t>(length * kRate);
    const auto attack = static_cast<std::size_t>(0.005 * kRate);
    const auto release = static_cast<std::size_t>(0.02 * kRate);
    for (std::size_t i = 0; i < count && first + i < out.size(); ++i) {
        const double turns = hz * static_cast<double>(i) / kRate;
        const double phase = turns - static_cast<double>(static_cast<std::int64_t>(turns));
        float envelope = 1.0F;
        if (pluck) {
            const float left = 1.0F - (static_cast<float>(i) / static_cast<float>(count));
            envelope = left * left;
        } else if (i + release > count) {
            envelope = static_cast<float>(count - i) / static_cast<float>(release);
        }
        if (i < attack) {
            envelope *= static_cast<float>(i) / static_cast<float>(attack);
        }
        const float value =
            volume * envelope * slash::cos_sin(static_cast<float>(kTwoPi * phase)).y;
        out[first + i].left += value;
        out[first + i].right += value;
    }
}

// Adds a hi-hat: 40 ms of noise dying away, from a fixed seed, its lows taken out by
// subtracting each random value from the one before.
void hat(std::vector<audio::Frame>& out, double start, std::uint32_t& seed) {
    const auto first = static_cast<std::size_t>(start * kRate);
    const auto count = static_cast<std::size_t>(0.04 * kRate);
    float last = 0.0F;
    for (std::size_t i = 0; i < count && first + i < out.size(); ++i) {
        seed = (seed * 1664525U) + 1013904223U;
        const float noise = (static_cast<float>(seed >> 8U) / 8388608.0F) - 1.0F;
        const float left = 1.0F - (static_cast<float>(i) / static_cast<float>(count));
        const float value = 0.05F * left * left * (noise - last);
        last = noise;
        out[first + i].left += value;
        out[first + i].right += value;
    }
}

std::vector<audio::Frame> groove() {
    std::vector<audio::Frame> out(static_cast<std::size_t>(kBeats * kBeat * kRate));
    // i - VI - iv - V in D minor: Dm, Bb, Gm and A, whose C# leads back to D.
    const int chords[4][3] = {{62, 65, 69}, {58, 62, 65}, {55, 58, 62}, {57, 61, 64}};
    const int roots[4] = {50, 46, 43, 45};
    std::uint32_t seed = 1;
    for (int beat = 0; beat < kBeats; ++beat) {
        const double at = beat * kBeat;
        const int bar = beat / 4;
        play(out, 31, at, 0.25, 0.5F, true); // kick
        hat(out, at + (kBeat / 2), seed);
        play(out, roots[bar], at, kBeat, 0.25F, true); // bass
        if (beat % 4 == 0) {
            for (const int midi : chords[bar]) {
                play(out, midi, at, 4 * kBeat, 0.08F, false);
            }
        }
    }
    return out;
}

void write(const std::string& path, const std::string& data) {
    if (!slash::platform::write_file(path, data)) {
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
    (void)slash::platform::create_directories("music");
    const std::vector<audio::Frame> music = groove();
    write("music/groove.wav", audio::encode_wav(music, kRate));
    // Turned up four times, every loud beat is cut flat at full scale.
    std::vector<audio::Frame> loud = music;
    for (audio::Frame& frame : loud) {
        frame.left = std::clamp(frame.left * 4.0F, -1.0F, 1.0F);
        frame.right = std::clamp(frame.right * 4.0F, -1.0F, 1.0F);
    }
    write("music/groove_loud.wav", audio::encode_wav(loud, kRate));
    // The whole groove loops; the other file's loop ends a tenth of a beat early, mid-note.
    write("music/groove.audio.json", R"({"format": 1, "source": "music/groove.wav",
        "loop_mode": "forward", "bpm": 100, "beat_count": 16})");
    write("music/groove_cut.audio.json", R"({"format": 1, "source": "music/groove.wav",
        "loop_mode": "forward", "loop_end": 420714, "bpm": 100, "beat_count": 16})");
    std::puts("wrote music/groove.wav, music/groove_loud.wav and two .audio.json files");

    slash::world::ComponentRegistry components;
    slash::world::register_builtin_components(components);
    command::Registry commands;
    command::register_builtin_commands(commands);
    command::SessionOptions options;
    options.project_root = ".";
    options.capture_logs = false;
    command::Session session(components, commands, options);

    // The groove as its .audio.json file plays it: 100 beats per minute and 16 beats long, as
    // the file says, in D minor, with the bass and kick's energy low and a clean loop.
    call(session, "audio.analyze", R"({"path": "music/groove.audio.json"})");
    // Cut mid-note, the loop jumps where it joins: a click.
    call(session, "audio.analyze", R"({"path": "music/groove_cut.audio.json"})");
    // Too loud: clipped beats, and a true peak over the -1 dBTP that EBU R 128 allows.
    call(session, "audio.analyze", R"({"path": "music/groove_loud.wav"})");

    // What the Master bus played over one turn of the loop, ten seconds in.
    call(session, "entity.create", R"({"id": "music", "components": {"AudioStreamPlayer":
        {"stream": "music/groove.audio.json", "autoplay": true}}})");
    call(session, "step", R"({"ticks": 600})");
    call(session, "audio.analyze", R"({"bus": "Master", "seconds": 9.6})");
    return 0;
}
