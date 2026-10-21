#include "slash/library/music.hpp"

#include "slash/audio/analysis.hpp"
#include "slash/audio/audio.hpp"

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace slash::library {
namespace {

constexpr std::array<std::string_view, 12> kPitchNames{"C",  "C#", "D",  "Eb", "E",  "F",
                                                       "F#", "G",  "Ab", "A",  "Bb", "B"};
// Tempos within 3% of each other, or of twice or half, are the same beat counted differently.
constexpr double kTempoTolerance = 0.03;
// Beats within a tenth of a whole number fill the sound, as music made to loop does.
constexpr double kWholeBeat = 0.1;

double rounded(double value, double places) {
    return std::round(value * places) / places;
}

std::string number(double value) {
    std::string text = std::to_string(rounded(value, 10.0));
    while (text.back() == '0') {
        text.pop_back();
    }
    if (text.back() == '.') {
        text.pop_back();
    }
    return text;
}

bool same_tempo(double a, double b, double ratio) {
    return std::abs((a / b) - ratio) <= kTempoTolerance * ratio;
}

// Whether two tempos are the same beat, or one counts it twice as often.
bool related_tempo(double a, double b) {
    return same_tempo(a, b, 1.0) || same_tempo(a, b, 2.0) || same_tempo(a, b, 0.5);
}

// Whether two keys share their notes: the same key, or its relative major or minor, which a
// measure often names instead.
bool related_key(Key a, Key b) {
    if (a.minor == b.minor) {
        return a.tonic == b.tonic;
    }
    const Key major = a.minor ? b : a;
    const Key minor = a.minor ? a : b;
    return (major.tonic + 9) % 12 == minor.tonic;
}

} // namespace

std::optional<Key> parse_key(std::string_view text) {
    while (!text.empty() && text.front() == ' ') {
        text.remove_prefix(1);
    }
    while (!text.empty() && text.back() == ' ') {
        text.remove_suffix(1);
    }
    if (text.empty()) {
        return std::nullopt;
    }
    constexpr std::array<int, 7> kNatural{9, 11, 0, 2, 4, 5, 7}; // A to G
    const char letter = text.front();
    const char upper =
        letter >= 'a' && letter <= 'g' ? static_cast<char>(letter - 'a' + 'A') : letter;
    if (upper < 'A' || upper > 'G') {
        return std::nullopt;
    }
    Key key{.tonic = kNatural.at(static_cast<std::size_t>(upper - 'A')), .minor = false};
    text.remove_prefix(1);
    if (!text.empty() && (text.front() == '#' || text.front() == 'b')) {
        key.tonic = (key.tonic + (text.front() == '#' ? 1 : 11)) % 12;
        text.remove_prefix(1);
    }
    while (!text.empty() && text.front() == ' ') {
        text.remove_prefix(1);
    }
    std::string mode;
    for (const char c : text) {
        mode += static_cast<char>(c >= 'A' && c <= 'Z' ? c - 'A' + 'a' : c);
    }
    if (mode == "minor" || mode == "min" || mode == "m") {
        key.minor = true;
    } else if (!mode.empty() && mode != "major" && mode != "maj") {
        return std::nullopt;
    }
    return key;
}

std::string key_name(Key key) {
    return std::string(kPitchNames.at(static_cast<std::size_t>(key.tonic % 12))) +
           (key.minor ? " minor" : " major");
}

json::Value bpm_json(double bpm) {
    if (bpm == std::floor(bpm) && std::abs(bpm) < 1e9) {
        return {static_cast<std::int64_t>(bpm)};
    }
    return {bpm};
}

Result<std::vector<std::string>> describe_music(std::string_view data, std::string_view path,
                                                const MusicFacts& given, bool trusted,
                                                Record& record) {
    auto sound = audio::decode_sound(data, path);
    if (!sound) {
        return sound.error();
    }
    auto frames = audio::decode_frames(sound.value());
    if (!frames) {
        return frames.error();
    }
    const audio::Analysis measured = audio::analyze(frames.value(), sound.value().mix_rate);
    const double seconds =
        static_cast<double>(frames.value().size()) / static_cast<double>(sound.value().mix_rate);
    std::vector<std::string> notes;

    record.type = "music";
    double bpm = given.bpm;
    if (measured.bpm) {
        const double heard = *measured.bpm;
        if (given.bpm > 0.0 && !related_tempo(heard, given.bpm)) {
            notes.push_back("it measures " + number(heard) + " bpm, not the " + number(given.bpm) +
                            " bpm " + (trusted ? "given" : "asked for"));
        }
        if (!trusted && (given.bpm <= 0.0 || !related_tempo(heard, given.bpm))) {
            bpm = heard;
        }
    }
    int beat_count = 0;
    if (bpm > 0.0 && seconds > 0.0) {
        const double beats = seconds * bpm / 60.0;
        const double whole = std::round(beats);
        if (trusted && given.beat_count > 0) {
            beat_count = given.beat_count;
            if (beat_count * 60.0 / bpm > seconds + 0.05) {
                notes.push_back(std::to_string(beat_count) + " beats at " + number(bpm) +
                                " bpm last " + number(beat_count * 60.0 / bpm) +
                                " s, longer than the sound's " + number(seconds) + " s");
            }
        } else if (whole >= 1.0 && std::abs(beats - whole) < kWholeBeat) {
            beat_count = static_cast<int>(whole);
            if (!trusted) {
                bpm = whole * 60.0 / seconds;
            }
        } else {
            beat_count = static_cast<int>(std::floor(beats));
        }
    }
    record.bpm = rounded(bpm, 1000.0);
    record.beat_count = beat_count;
    record.bar_beats = given.bar_beats;

    const std::optional<Key> asked = parse_key(given.key);
    std::optional<Key> heard;
    if (measured.key) {
        heard = parse_key(*measured.key);
    }
    std::optional<Key> key = asked;
    if (heard && asked && !related_key(*heard, *asked)) {
        notes.push_back("it measures in " + key_name(*heard) + ", not the " + key_name(*asked) +
                        " " + (trusted ? "given" : "asked for"));
        if (!trusted) {
            key = heard;
        }
    } else if (heard && !asked) {
        key = heard;
    }
    record.key = key ? key_name(*key) : std::string();
    return notes;
}

} // namespace slash::library
