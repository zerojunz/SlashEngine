// Music in the asset library: generated from the storyboard's theme, or a composer's own track
// with its stems, kept with its tempo, beats, key, author and license.
//
// The game's storyboard names a theme for its harbor: G major, 96 beats per minute, three beats
// to a bar, cello and harp. The agent asks for music in it, gets a loop that fits and the
// .audio.json settings to play it, then a slower version of it. A composer, Ana, delivers a boss
// fight with its drums and pads as stems (made in this program with integer math, so the files
// and their IDs are the same everywhere; no audio files are committed); the agent adds it with
// her name and license, and lays it out as layers that play together. The built-in "test"
// generator makes the music, so the example needs no model or key; a project names a music
// service in providers.json and calls it the same way.

#include "slash/audio/audio.hpp"
#include "slash/command/builtin.hpp"
#include "slash/command/command.hpp"
#include "slash/command/session.hpp"
#include "slash/core/json.hpp"
#include "slash/library/commands.hpp"
#include "slash/platform/file.hpp"
#include "slash/world/component_registry.hpp"
#include "slash/world/components.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
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
constexpr int kBeat = 18900; // frames: 140 beats per minute
constexpr int kBeats = 16;   // four bars of four

// Equal-tempered C4 to B4 in millihertz.
constexpr std::array<std::int64_t, 12> kOctave4{261626, 277183, 293665, 311127, 329628, 349228,
                                                369994, 391995, 415305, 440000, 466164, 493883};

std::int64_t millihertz(int midi) {
    std::int64_t value = kOctave4.at(static_cast<std::size_t>(midi % 12));
    for (int octave = (midi / 12) - 1; octave > 4; --octave) {
        value *= 2;
    }
    for (int octave = (midi / 12) - 1; octave < 4; ++octave) {
        value /= 2;
    }
    return value;
}

// A triangle wave at a phase in 65536ths of a turn, from -32768 to 32767.
std::int64_t triangle(std::int64_t phase) {
    const std::int64_t p = phase & 0xFFFF;
    return p < 32768 ? (p * 2) - 32768 : 98303 - (p * 2);
}

// Adds a note from frame start for count frames at volume thousandths: rising over 5 ms, then
// dying away to nothing at its end (a pluck) or holding until its last 20 ms (a pad).
void play(std::vector<std::int64_t>& out, int midi, int start, int count, int volume, bool pluck) {
    const std::int64_t hz = millihertz(midi);
    constexpr std::int64_t kAttack = kRate / 200;
    constexpr std::int64_t kRelease = kRate / 50;
    const std::int64_t n = count;
    for (std::int64_t i = 0; i < n && start + i < static_cast<std::int64_t>(out.size()); ++i) {
        std::int64_t envelope = 65536;
        if (pluck) {
            envelope = (n - i) * (n - i) * 65536 / (n * n);
        } else if (i + kRelease > n) {
            envelope = (n - i) * 65536 / kRelease;
        }
        if (i < kAttack) {
            envelope = envelope * i / kAttack;
        }
        const std::int64_t phase = i * hz * 65536 / (kRate * 1000LL);
        out[static_cast<std::size_t>(start + i)] +=
            triangle(phase) * volume * envelope / (1000LL * 65536);
    }
}

// Adds a hi-hat: 40 ms of noise dying away, its lows taken out by subtracting each random value
// from the one before.
void hat(std::vector<std::int64_t>& out, int start, std::uint32_t& seed) {
    constexpr std::int64_t kCount = kRate / 25;
    std::int64_t last = 0;
    for (std::int64_t i = 0; i < kCount; ++i) {
        seed = (seed * 1664525U) + 1013904223U;
        const std::int64_t noise = static_cast<std::int64_t>(seed >> 16U) - 32768;
        const std::int64_t left = kCount - i;
        out[static_cast<std::size_t>(start + i)] +=
            (noise - last) * 40 * left * left / (1000LL * kCount * kCount);
        last = noise;
    }
}

std::string wav(const std::vector<std::int64_t>& samples) {
    std::vector<audio::Frame> frames;
    frames.reserve(samples.size());
    for (const std::int64_t sample : samples) {
        const float value =
            static_cast<float>(std::clamp<std::int64_t>(sample, -32768, 32767)) / 32768.0F;
        frames.push_back({.left = value, .right = value});
    }
    return audio::encode_wav(frames, kRate);
}

// Ana's boss fight in E minor: kick and hi-hat on the drums stem, bass and chords (i - VI - III
// - VII: Em, C, G and D) on the pads stem, and the mix of the two.
void write_delivery() {
    std::vector<std::int64_t> drums(static_cast<std::size_t>(kBeats) * kBeat);
    std::vector<std::int64_t> pads(drums.size());
    const int chords[4][3] = {{64, 67, 71}, {60, 64, 67}, {62, 67, 71}, {62, 66, 69}};
    const int roots[4] = {40, 36, 43, 38};
    std::uint32_t seed = 7;
    for (int beat = 0; beat < kBeats; ++beat) {
        const int at = beat * kBeat;
        const int bar = beat / 4;
        play(drums, 31, at, kRate / 4, 500, true); // kick
        hat(drums, at + (kBeat / 2), seed);
        play(pads, roots[bar], at, kBeat, 250, true); // bass
        if (beat % 4 == 0) {
            for (const int midi : chords[bar]) {
                play(pads, midi, at, 4 * kBeat, 80, false);
            }
        }
    }
    std::vector<std::int64_t> mix(drums.size());
    for (std::size_t i = 0; i < mix.size(); ++i) {
        mix[i] = drums[i] + pads[i];
    }
    (void)slash::platform::create_directories("game/delivery");
    (void)slash::platform::write_file("game/delivery/boss.wav", wav(mix));
    (void)slash::platform::write_file("game/delivery/boss_drums.wav", wav(drums));
    (void)slash::platform::write_file("game/delivery/boss_pads.wav", wav(pads));
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

void show(const std::string& path) {
    const json::Value value =
        json::parse(slash::platform::read_file("game/" + path).value()).value();
    std::printf("%s: %s\n", path.c_str(), json::write(value, json::Style::Compact).c_str());
}

} // namespace

int main() {
    write_delivery();
    std::puts("wrote game/delivery/boss.wav, boss_drums.wav and boss_pads.wav");

    slash::world::ComponentRegistry components;
    slash::world::register_builtin_components(components);
    command::Registry commands;
    command::register_builtin_commands(commands);
    slash::library::register_commands(commands);

    // The library would be ~/.slash/library; the example keeps its own.
    command::SessionOptions options;
    options.capture_logs = false;
    options.library_dir = "library";
    options.project_root = "game";
    command::Session game(components, commands, options);

    // Music in the storyboard's theme: its key, tempo and meter, and its instruments and mood
    // in the prompt. The reply says what the music measures, in whole bars that loop, and the
    // settings file written beside it.
    const std::string harbor = R"({"type": "music", "prompt": "The harbor at dawn.",
        "generator": "test", "theme": "theme/harbor", "seconds": 8, "path": "music/harbor.wav",
        "loop": true})";
    const json::Value made = call(game, "asset.generate", harbor);
    show("music/harbor.audio.json");
    // Asked again, it comes from the library.
    call(game, "asset.generate", harbor);
    // A slower version for the night, linked to the first.
    const std::string id = made.as_object().at("id").as_string();
    call(game, "asset.refine", R"({"id": ")" + id + R"(", "prompt": "slower",
        "description": "The harbor at night.", "path": "music/harbor_night.wav", "loop": true})");
    call(game, "asset.generate", R"({"type": "music", "prompt": "x", "generator": "test",
        "theme": "theme/harbour"})");

    // Ana's track, with her tempo and key, her name and license, and its stems.
    const json::Value added = call(game, "asset.add", R"({"path": "delivery/boss.wav",
        "description": "Boss fight, drums and pads.", "author": "Ana", "license": "CC-BY-4.0",
        "bpm": 140, "key": "E minor", "tags": ["boss"],
        "stems": {"drums": "delivery/boss_drums.wav", "pads": "delivery/boss_pads.wav"}})");
    call(game, "asset.search", R"({"type": "music", "tags": ["boss"]})");
    // Laid out to play: each stem with its settings, and one stream that plays them together.
    const std::string boss = added.as_object().at("id").as_string();
    call(game, "asset.use",
         R"({"id": ")" + boss + R"(", "path": "music/boss.wav", "stems": true, "loop": true})");
    show("music/boss.audio.json");
    show("music/boss/drums.audio.json");

    // Played, the stems sound as the mix: 140 beats per minute, measured in G major, which has
    // E minor's notes.
    call(game, "entity.create", R"({"id": "music", "components": {"AudioStreamPlayer":
        {"stream": "music/boss.audio.json", "autoplay": true}}})");
    call(game, "step", R"({"ticks": 420})");
    call(game, "audio.analyze", R"({"bus": "Master", "seconds": 6.857})");

    // Where each file came from, and under what license.
    show("library.json");
    return 0;
}
