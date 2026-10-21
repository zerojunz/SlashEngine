#include "slash/soundtrack/score.hpp"

#include "slash/core/text.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <map>
#include <numeric>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

namespace slash::soundtrack {
namespace {

// General MIDI's instruments in program order.
constexpr std::array<std::string_view, 128> kInstruments = {
    "acoustic_grand_piano",
    "bright_acoustic_piano",
    "electric_grand_piano",
    "honky_tonk_piano",
    "electric_piano_1",
    "electric_piano_2",
    "harpsichord",
    "clavinet",
    "celesta",
    "glockenspiel",
    "music_box",
    "vibraphone",
    "marimba",
    "xylophone",
    "tubular_bells",
    "dulcimer",
    "drawbar_organ",
    "percussive_organ",
    "rock_organ",
    "church_organ",
    "reed_organ",
    "accordion",
    "harmonica",
    "tango_accordion",
    "acoustic_guitar_nylon",
    "acoustic_guitar_steel",
    "electric_guitar_jazz",
    "electric_guitar_clean",
    "electric_guitar_muted",
    "overdriven_guitar",
    "distortion_guitar",
    "guitar_harmonics",
    "acoustic_bass",
    "electric_bass_finger",
    "electric_bass_pick",
    "fretless_bass",
    "slap_bass_1",
    "slap_bass_2",
    "synth_bass_1",
    "synth_bass_2",
    "violin",
    "viola",
    "cello",
    "contrabass",
    "tremolo_strings",
    "pizzicato_strings",
    "orchestral_harp",
    "timpani",
    "string_ensemble_1",
    "string_ensemble_2",
    "synth_strings_1",
    "synth_strings_2",
    "choir_aahs",
    "voice_oohs",
    "synth_voice",
    "orchestra_hit",
    "trumpet",
    "trombone",
    "tuba",
    "muted_trumpet",
    "french_horn",
    "brass_section",
    "synth_brass_1",
    "synth_brass_2",
    "soprano_sax",
    "alto_sax",
    "tenor_sax",
    "baritone_sax",
    "oboe",
    "english_horn",
    "bassoon",
    "clarinet",
    "piccolo",
    "flute",
    "recorder",
    "pan_flute",
    "blown_bottle",
    "shakuhachi",
    "whistle",
    "ocarina",
    "lead_square",
    "lead_sawtooth",
    "lead_calliope",
    "lead_chiff",
    "lead_charang",
    "lead_voice",
    "lead_fifths",
    "lead_bass",
    "pad_new_age",
    "pad_warm",
    "pad_polysynth",
    "pad_choir",
    "pad_bowed",
    "pad_metallic",
    "pad_halo",
    "pad_sweep",
    "fx_rain",
    "fx_soundtrack",
    "fx_crystal",
    "fx_atmosphere",
    "fx_brightness",
    "fx_goblins",
    "fx_echoes",
    "fx_sci_fi",
    "sitar",
    "banjo",
    "shamisen",
    "koto",
    "kalimba",
    "bagpipe",
    "fiddle",
    "shanai",
    "tinkle_bell",
    "agogo",
    "steel_drums",
    "woodblock",
    "taiko_drum",
    "melodic_tom",
    "synth_drum",
    "reverse_cymbal",
    "guitar_fret_noise",
    "breath_noise",
    "seashore",
    "bird_tweet",
    "telephone_ring",
    "helicopter",
    "applause",
    "gunshot",
};

// General MIDI's percussion key map, from note 35.
constexpr std::array<std::string_view, 47> kDrums = {
    "acoustic_kick",
    "kick",
    "side_stick",
    "snare",
    "clap",
    "electric_snare",
    "low_floor_tom",
    "closed_hat",
    "high_floor_tom",
    "pedal_hat",
    "low_tom",
    "open_hat",
    "low_mid_tom",
    "high_mid_tom",
    "crash",
    "high_tom",
    "ride",
    "china",
    "ride_bell",
    "tambourine",
    "splash",
    "cowbell",
    "crash_2",
    "vibraslap",
    "ride_2",
    "high_bongo",
    "low_bongo",
    "mute_high_conga",
    "open_high_conga",
    "low_conga",
    "high_timbale",
    "low_timbale",
    "high_agogo",
    "low_agogo",
    "cabasa",
    "maracas",
    "short_whistle",
    "long_whistle",
    "short_guiro",
    "long_guiro",
    "claves",
    "high_wood_block",
    "low_wood_block",
    "mute_cuica",
    "open_cuica",
    "mute_triangle",
    "open_triangle",
};

constexpr std::string_view kDrumKit = "drums";
constexpr std::string_view kDefaultInstrument = "acoustic_grand_piano";
constexpr std::array<std::string_view, 12> kSharpNames = {"C",  "C#", "D",  "D#", "E",  "F",
                                                          "F#", "G",  "G#", "A",  "A#", "B"};
constexpr std::array<std::string_view, 12> kFlatNames = {"C",  "Db", "D",  "Eb", "E",  "F",
                                                         "Gb", "G",  "Ab", "A",  "Bb", "B"};
// Keys by signature, from seven flats to seven sharps.
constexpr std::array<std::string_view, 15> kMajorKeys = {
    "Cb", "Gb", "Db", "Ab", "Eb", "Bb", "F", "C", "G", "D", "A", "E", "B", "F#", "C#"};
constexpr std::array<std::string_view, 15> kMinorKeys = {
    "Ab", "Eb", "Bb", "F", "C", "G", "D", "A", "E", "B", "F#", "C#", "G#", "D#", "A#"};
constexpr int kMaxSharps = 7;

constexpr double kDefaultBpm = 120.0;
constexpr int kDefaultBarBeats = 4;
constexpr int kDefaultBeatUnit = 4;
constexpr int kDefaultVelocity = 100;
constexpr int kMaxBars = 1024;
constexpr int kMaxTracks = 64;
constexpr double kMaxLength = 1024.0;
// Lengths and positions within this much of a beat are the same: fractions such as 1/3 don't
// add up exactly in floating point.
constexpr double kSlack = 1.0e-6;

int letter_class(char letter) {
    switch (letter) {
    case 'C':
        return 0;
    case 'D':
        return 2;
    case 'E':
        return 4;
    case 'F':
        return 5;
    case 'G':
        return 7;
    case 'A':
        return 9;
    case 'B':
        return 11;
    default:
        return -1;
    }
}

// The pitch class of a note name with at most one accidental, such as "F#" or "Bb".
int name_class(std::string_view name) {
    int pitch = letter_class(name.front());
    if (name.size() == 2) {
        pitch += name[1] == '#' ? 1 : -1;
    }
    return (pitch + 12) % 12;
}

bool is_digit(char c) {
    return c >= '0' && c <= '9';
}

// A whole number of up to six digits.
std::optional<int> parse_int(std::string_view text) {
    if (text.empty() || text.size() > 6 || !std::ranges::all_of(text, is_digit)) {
        return std::nullopt;
    }
    int value = 0;
    for (const char c : text) {
        value = (value * 10) + (c - '0');
    }
    return value;
}

// Beats as 1, 0.5 or 3/2.
std::optional<double> parse_beats(std::string_view text) {
    if (const std::size_t slash = text.find('/'); slash != std::string_view::npos) {
        const auto top = parse_int(text.substr(0, slash));
        const auto bottom = parse_int(text.substr(slash + 1));
        if (!top || !bottom || *bottom == 0) {
            return std::nullopt;
        }
        return static_cast<double>(*top) / *bottom;
    }
    const std::size_t dot = text.find('.');
    const auto whole = parse_int(text.substr(0, dot));
    if (!whole) {
        return std::nullopt;
    }
    if (dot == std::string_view::npos) {
        return *whole;
    }
    const std::string_view decimals = text.substr(dot + 1);
    const auto part = parse_int(decimals);
    if (!part) {
        return std::nullopt;
    }
    return *whole + (*part / std::pow(10.0, static_cast<double>(decimals.size())));
}

// A number of beats in replies and errors: whole, or to three decimals.
std::string beats_text(double beats) {
    const double rounded = std::round(beats * 1000.0) / 1000.0;
    if (rounded == std::floor(rounded)) {
        return std::to_string(static_cast<std::int64_t>(rounded));
    }
    std::string text = std::to_string(rounded);
    while (text.back() == '0') {
        text.pop_back();
    }
    return text;
}

// A length of units 1/grid of a beat each: whole, a decimal of halves, quarters, eighths or
// sixteenths, else a fraction.
std::string units_text(std::int64_t units, int grid) {
    const std::int64_t divisor = std::gcd(units, static_cast<std::int64_t>(grid));
    const std::int64_t top = units / divisor;
    const std::int64_t bottom = grid / divisor;
    if (bottom == 1) {
        return std::to_string(top);
    }
    if (bottom == 2 || bottom == 4 || bottom == 8 || bottom == 16) {
        return beats_text(static_cast<double>(top) / static_cast<double>(bottom));
    }
    return std::to_string(top) + "/" + std::to_string(bottom);
}

std::string quoted(std::string_view text) {
    return "\"" + std::string(text) + "\"";
}

json::Value whole_or_real(double value) {
    if (value == std::floor(value) && std::abs(value) < 1.0e9) {
        return static_cast<std::int64_t>(value);
    }
    return value;
}

json::Value number(std::string_view doc, double minimum, double maximum, double fallback) {
    json::Value value;
    value["type"] = "number";
    value["minimum"] = minimum;
    value["maximum"] = maximum;
    value["default"] = whole_or_real(fallback);
    value["description"] = doc;
    return value;
}

json::Value integer(std::string_view doc, int minimum, int maximum, int fallback) {
    json::Value value;
    value["type"] = "integer";
    value["minimum"] = minimum;
    value["maximum"] = maximum;
    value["default"] = fallback;
    value["description"] = doc;
    return value;
}

json::Value text(std::string_view doc, int max_length) {
    json::Value value;
    value["type"] = "string";
    value["minLength"] = 1;
    value["maxLength"] = max_length;
    value["description"] = doc;
    return value;
}

// Why a pitch didn't read, with the closest drum for a misspelled one.
std::string pitch_error(std::string_view name, bool drums) {
    if (!drums && parse_pitch(name, true)) {
        return quoted(name) + " is a drum; drums play on a track whose instrument is \"drums\"";
    }
    std::string message = quoted(name) + " isn't a note: write A to G with # or b and an octave, "
                                         "such as C4 or Bb2, or a MIDI note number";
    if (drums) {
        message += ", or a drum such as kick, snare or closed_hat";
        if (const std::string_view near = closest_match(name, kDrums); !near.empty()) {
            message += "; did you mean " + quoted(near) + "?";
        }
    }
    return message;
}

int score_length(const json::Value& doc) {
    if (const json::Value* bars = doc.find("bars"); bars != nullptr) {
        return static_cast<int>(bars->as_int());
    }
    std::size_t longest = 0;
    for (const json::Value& track : doc.find("tracks")->as_array()) {
        longest = std::max(longest, track.find("bars")->size());
    }
    return static_cast<int>(longest);
}

template <typename T>
T value_or(const json::Value& doc, std::string_view key, T fallback) {
    const json::Value* found = doc.find(key);
    if (found == nullptr) {
        return fallback;
    }
    if constexpr (std::is_same_v<T, int>) {
        return static_cast<int>(found->as_int());
    } else if constexpr (std::is_same_v<T, double>) {
        return found->as_double();
    } else {
        return found->as_string();
    }
}

} // namespace

std::span<const std::string_view> instrument_names() {
    return kInstruments;
}

std::optional<int> instrument_program(std::string_view name) {
    const auto index = std::ranges::find(kInstruments, name) - kInstruments.begin();
    if (index == std::ssize(kInstruments)) {
        return std::nullopt;
    }
    return static_cast<int>(index);
}

std::span<const std::string_view> drum_names() {
    return kDrums;
}

std::optional<int> parse_pitch(std::string_view name, bool drums) {
    if (name.empty()) {
        return std::nullopt;
    }
    if (is_digit(name.front())) {
        const auto number = parse_int(name);
        if (!number || *number > 127) {
            return std::nullopt;
        }
        return number;
    }
    if (const int letter = letter_class(name.front()); letter >= 0) {
        int pitch = letter;
        std::size_t i = 1;
        while (i < name.size() && (name[i] == '#' || name[i] == 'b') && i < 3) {
            pitch += name[i] == '#' ? 1 : -1;
            ++i;
        }
        bool below = false;
        if (i < name.size() && name[i] == '-') {
            below = true;
            ++i;
        }
        const auto octave = parse_int(name.substr(i));
        if (!octave || *octave > 9 || (below && *octave != 1)) {
            return std::nullopt;
        }
        pitch += 12 * ((below ? -1 : *octave) + 1);
        if (pitch < 0 || pitch > 127) {
            return std::nullopt;
        }
        return pitch;
    }
    if (drums) {
        if (const auto index = std::ranges::find(kDrums, name) - kDrums.begin();
            index != std::ssize(kDrums)) {
            return kFirstDrum + static_cast<int>(index);
        }
    }
    return std::nullopt;
}

std::string pitch_name(int pitch, bool flats, bool drums) {
    if (drums && pitch >= kFirstDrum && pitch < kFirstDrum + static_cast<int>(kDrums.size())) {
        return std::string(kDrums[static_cast<std::size_t>(pitch - kFirstDrum)]);
    }
    const auto names = flats ? kFlatNames : kSharpNames;
    return std::string(names[static_cast<std::size_t>(pitch % 12)]) +
           std::to_string((pitch / 12) - 1);
}

std::optional<Key> parse_key(std::string_view text) {
    const std::size_t space = text.find(' ');
    if (space == std::string_view::npos) {
        return std::nullopt;
    }
    const std::string_view tonic = text.substr(0, space);
    const std::string_view mode = text.substr(space + 1);
    if (mode != "major" && mode != "minor") {
        return std::nullopt;
    }
    if (tonic.empty() || tonic.size() > 2 || letter_class(tonic.front()) < 0 ||
        (tonic.size() == 2 && tonic[1] != '#' && tonic[1] != 'b')) {
        return std::nullopt;
    }
    Key key;
    key.minor = mode == "minor";
    key.tonic = name_class(tonic);
    const auto& keys = key.minor ? kMinorKeys : kMajorKeys;
    // The signature it is spelled with, else the plainest one with the same tonic.
    int best = kMaxSharps + 1;
    for (std::size_t i = 0; i < keys.size(); ++i) {
        const int sharps = static_cast<int>(i) - kMaxSharps;
        if (keys[i] == tonic) {
            best = sharps;
            break;
        }
        if (name_class(keys[i]) == key.tonic && std::abs(sharps) < std::abs(best)) {
            best = sharps;
        }
    }
    key.sharps = best;
    return key;
}

std::string key_name(int sharps, bool minor) {
    const int index = std::clamp(sharps, -kMaxSharps, kMaxSharps) + kMaxSharps;
    const auto& keys = minor ? kMinorKeys : kMajorKeys;
    return std::string(keys[static_cast<std::size_t>(index)]) + (minor ? " minor" : " major");
}

BarNotes decode_bar(std::string_view text, int bar_beats, double start, int velocity, bool drums) {
    BarNotes out;
    std::vector<std::string_view> voices;
    if (text.find_first_not_of(' ') == std::string_view::npos) {
        return out;
    }
    for (std::size_t from = 0;;) {
        const std::size_t to = text.find(';', from);
        voices.push_back(text.substr(from, to - from));
        if (to == std::string_view::npos) {
            break;
        }
        from = to + 1;
    }
    const auto bar_end = static_cast<double>(bar_beats);
    for (std::size_t v = 0; v < voices.size(); ++v) {
        const std::string where = voices.size() > 1 ? "voice " + std::to_string(v + 1) + ", " : "";
        std::vector<std::string_view> tokens;
        for (std::size_t i = 0; i < voices[v].size();) {
            const std::size_t first = voices[v].find_first_not_of(' ', i);
            if (first == std::string_view::npos) {
                break;
            }
            const std::size_t last = std::min(voices[v].find(' ', first), voices[v].size());
            tokens.push_back(voices[v].substr(first, last - first));
            i = last;
        }
        if (tokens.empty()) {
            out.error = "voice " + std::to_string(v + 1) + " is empty";
            return out;
        }
        double cursor = 0.0;
        double length = 1.0;
        for (std::size_t t = 0; t < tokens.size(); ++t) {
            const std::string_view token = tokens[t];
            const auto fail = [&](const std::string& why) {
                out.error = where;
                out.error += quoted(token);
                out.error += ": ";
                out.error += why;
                return out;
            };
            const std::size_t at = token.find('@');
            const std::size_t colon = token.find(':');
            if (at != std::string_view::npos && colon != std::string_view::npos && at < colon) {
                return fail("the length goes before the velocity, as in C4:1@90");
            }
            const std::string_view head = token.substr(0, std::min(at, colon));
            if (colon != std::string_view::npos) {
                const std::string_view written =
                    token.substr(colon + 1, at == std::string_view::npos ? std::string_view::npos
                                                                         : at - colon - 1);
                const auto beats = parse_beats(written);
                if (!beats || *beats <= 0.0 || *beats > kMaxLength) {
                    return fail("a length is a number of beats above 0, such as 1, 0.5 or 1/3");
                }
                length = *beats;
            }
            int loudness = velocity;
            if (at != std::string_view::npos) {
                const auto written = parse_int(token.substr(at + 1));
                if (!written || *written < 1 || *written > 127) {
                    return fail("a velocity is 1 to 127");
                }
                loudness = *written;
            }
            const bool rest = head == "r";
            std::vector<int> pitches;
            if (!rest) {
                if (head.empty()) {
                    return fail("needs a note, a chord or r for a rest");
                }
                for (std::size_t from = 0;;) {
                    const std::size_t to = head.find('+', from);
                    const std::string_view name = head.substr(from, to - from);
                    const auto pitch = parse_pitch(name, drums);
                    if (!pitch) {
                        return fail(pitch_error(name, drums));
                    }
                    pitches.push_back(*pitch);
                    if (to == std::string_view::npos) {
                        break;
                    }
                    from = to + 1;
                }
            } else if (at != std::string_view::npos) {
                return fail("a rest has no velocity");
            }
            if (cursor >= bar_end - kSlack) {
                return fail("starts at beat " + beats_text(cursor) + ", after the bar's " +
                            std::to_string(bar_beats) + " beats");
            }
            const double end = cursor + length;
            if (end > bar_end + kSlack && (rest || t + 1 < tokens.size())) {
                return fail("runs to beat " + beats_text(end) + ", past the bar's " +
                            std::to_string(bar_beats) +
                            " beats; only a voice's last note can ring past the bar line");
            }
            for (const int pitch : pitches) {
                out.notes.push_back({.beat = start + cursor,
                                     .length = length,
                                     .pitch = pitch,
                                     .velocity = loudness});
            }
            cursor = end;
        }
    }
    std::ranges::sort(out.notes, [](const Note& a, const Note& b) {
        return a.beat != b.beat ? a.beat < b.beat : a.pitch < b.pitch;
    });
    return out;
}

std::vector<std::string> encode_bars(std::span<const Note> notes, int bar_beats, int velocity,
                                     bool drums, bool flats, int grid) {
    struct Snapped {
        std::int64_t start = 0;
        std::int64_t length = 0;
        int velocity = 0;
        int pitch = 0;
    };
    const std::int64_t bar_units = static_cast<std::int64_t>(bar_beats) * grid;
    std::map<std::int64_t, std::vector<Snapped>> by_bar;
    for (const Note& note : notes) {
        const std::int64_t start = std::max<std::int64_t>(0, std::llround(note.beat * grid));
        const std::int64_t length = std::max<std::int64_t>(1, std::llround(note.length * grid));
        by_bar[start / bar_units].push_back(
            {start % bar_units, length, std::clamp(note.velocity, 1, 127), note.pitch});
    }
    std::vector<std::string> bars;
    if (by_bar.empty()) {
        return bars;
    }
    bars.resize(static_cast<std::size_t>(by_bar.rbegin()->first + 1));
    for (auto& [bar, in_bar] : by_bar) {
        // Longest first at each start, so a held note takes the first voice; then chords.
        std::ranges::sort(in_bar, [](const Snapped& a, const Snapped& b) {
            if (a.start != b.start) {
                return a.start < b.start;
            }
            if (a.length != b.length) {
                return a.length > b.length;
            }
            return a.velocity != b.velocity ? a.velocity < b.velocity : a.pitch < b.pitch;
        });
        struct Chord {
            std::int64_t start = 0;
            std::int64_t length = 0;
            int velocity = 0;
            std::vector<int> pitches;
        };
        struct Voice {
            std::int64_t cursor = 0;
            bool closed = false; // its last chord rings past the bar line
            std::vector<Chord> chords;
        };
        std::vector<Voice> voices;
        for (std::size_t i = 0; i < in_bar.size();) {
            Chord chord{in_bar[i].start, in_bar[i].length, in_bar[i].velocity, {}};
            while (i < in_bar.size() && in_bar[i].start == chord.start &&
                   in_bar[i].length == chord.length && in_bar[i].velocity == chord.velocity) {
                if (std::ranges::find(chord.pitches, in_bar[i].pitch) == chord.pitches.end()) {
                    chord.pitches.push_back(in_bar[i].pitch);
                }
                ++i;
            }
            auto voice = std::ranges::find_if(voices, [&](const Voice& each) {
                return !each.closed && each.cursor <= chord.start;
            });
            if (voice == voices.end()) {
                voices.emplace_back();
                voice = voices.end() - 1;
            }
            voice->cursor = chord.start + chord.length;
            voice->closed = voice->cursor > bar_units;
            voice->chords.push_back(std::move(chord));
        }
        std::string text;
        for (const Voice& voice : voices) {
            if (!text.empty()) {
                text += " ; ";
            }
            std::int64_t cursor = 0;
            std::int64_t length = grid;
            std::string line;
            const auto add = [&](const std::string& head, std::int64_t units, int loudness) {
                if (!line.empty()) {
                    line += ' ';
                }
                line += head;
                if (units != length) {
                    line += ":" + units_text(units, grid);
                    length = units;
                }
                if (loudness != velocity && loudness > 0) {
                    line += "@" + std::to_string(loudness);
                }
            };
            for (const Chord& chord : voice.chords) {
                if (chord.start > cursor) {
                    add("r", chord.start - cursor, 0);
                }
                std::string head;
                for (const int pitch : chord.pitches) {
                    if (!head.empty()) {
                        head += '+';
                    }
                    head += pitch_name(pitch, flats, drums);
                }
                add(head, chord.length, chord.velocity);
                cursor = chord.start + chord.length;
            }
            text += line;
        }
        bars[static_cast<std::size_t>(bar)] = std::move(text);
    }
    return bars;
}

json::Value score_schema() {
    json::Value schema;
    schema["$schema"] = "https://json-schema.org/draft/2020-12/schema";
    schema["title"] = "SlashEngine score";
    schema["description"] =
        "A piece of music as text: tracks of notes in beats, with a tempo, meter, key and named "
        "sections. Each track holds a line of text per bar: notes such as \"C4\", \"F#3\" or "
        "\"Bb2\" (or MIDI note numbers, or drums by name) played one after another from the "
        "start of the bar, each with :length in beats (\"D3:0.5\", carried on to the notes after "
        "it) and @velocity; \"+\" joins a chord, \"r\" is a rest and \";\" starts another voice.";
    schema["type"] = "object";
    schema["additionalProperties"] = false;
    schema["required"] = json::Array{"format", "tracks"};
    json::Value& properties = schema["properties"];
    properties["format"]["const"] = 1;
    properties["format"]["description"] = "Format version. Must be 1.";
    properties["bpm"] = number("Beats per minute.", 1, 400, kDefaultBpm);
    properties["bar_beats"] = integer("Beats in a bar.", 2, 32, kDefaultBarBeats);
    json::Value& unit = properties["beat_unit"];
    unit["enum"] = json::Array{2, 4, 8, 16};
    unit["default"] = kDefaultBeatUnit;
    unit["description"] = "The note a beat is, as a time signature's lower number: 4 for a "
                          "quarter note, 8 for an eighth.";
    properties["key"]["type"] = "string";
    properties["key"]["description"] = R"(The key, such as "D minor" or "F# major".)";
    json::Value& bars = properties["bars"];
    bars["type"] = "integer";
    bars["minimum"] = 1;
    bars["maximum"] = kMaxBars;
    bars["description"] = "Its length in bars. Default: the longest track's.";
    json::Value& sections = properties["sections"];
    sections["type"] = "array";
    sections["maxItems"] = 256;
    sections["description"] = "Named parts in order, each from its bar to the next one's.";
    json::Value& section = sections["items"];
    section["type"] = "object";
    section["additionalProperties"] = false;
    section["required"] = json::Array{"name", "bar"};
    section["properties"]["name"] = text(R"(Its name, such as "intro" or "loop".)", 64);
    section["properties"]["bar"]["type"] = "integer";
    section["properties"]["bar"]["minimum"] = 0;
    section["properties"]["bar"]["description"] =
        "The bar it starts at, counted from 0 as a track's bars are.";
    json::Value& tracks = properties["tracks"];
    tracks["type"] = "array";
    tracks["minItems"] = 1;
    tracks["maxItems"] = kMaxTracks;
    tracks["description"] = "The instruments' parts.";
    json::Value& track = tracks["items"];
    track["type"] = "object";
    track["additionalProperties"] = false;
    track["required"] = json::Array{"name", "bars"};
    json::Value& fields = track["properties"];
    fields["name"] = text("Its name, unique in the score.", 64);
    fields["instrument"]["type"] = "string";
    fields["instrument"]["default"] = kDefaultInstrument;
    fields["instrument"]["description"] =
        "A General MIDI instrument, such as \"string_ensemble_1\", \"electric_bass_finger\" or "
        "\"flute\", or \"drums\" for General MIDI's drums, named as kick, snare, closed_hat, "
        "open_hat, crash and the rest.";
    fields["volume_db"] = number("Its volume.", -80, 0, 0);
    fields["pan"] = number("Where it sits, from -1 (left) to 1 (right).", -1, 1, 0);
    fields["velocity"] =
        integer("What its notes play at unless they say.", 1, 127, kDefaultVelocity);
    json::Value& lines = fields["bars"];
    lines["type"] = "array";
    lines["maxItems"] = kMaxBars;
    lines["items"]["type"] = "string";
    lines["description"] = "A line of notes per bar, \"\" for a silent bar.";
    return schema;
}

std::vector<json::ValidationError> validate_score(const json::Value& doc) {
    std::vector<json::ValidationError> errors = json::validate(doc, score_schema());
    if (!errors.empty()) {
        return errors;
    }
    if (const json::Value* key = doc.find("key"); key != nullptr && !parse_key(key->as_string())) {
        errors.push_back({"/key", R"(expected a key such as "D minor" or "F# major")"});
    }
    const int bar_beats = value_or(doc, "bar_beats", kDefaultBarBeats);
    const int length = score_length(doc);
    const json::Array& tracks = doc.find("tracks")->as_array();
    std::set<std::string_view> names;
    for (std::size_t i = 0; i < tracks.size(); ++i) {
        const std::string at = "/tracks/" + std::to_string(i);
        const json::Value& track = tracks[i];
        const std::string& name = track.find("name")->as_string();
        if (!names.insert(name).second) {
            errors.push_back({at + "/name", "another track is called " + quoted(name)});
        }
        const std::string instrument =
            value_or(track, "instrument", std::string(kDefaultInstrument));
        const bool drums = instrument == kDrumKit;
        if (!drums && !instrument_program(instrument)) {
            std::string message = "no General MIDI instrument " + quoted(instrument);
            std::vector<std::string_view> candidates(kInstruments.begin(), kInstruments.end());
            candidates.push_back(kDrumKit);
            if (const std::string_view near = closest_match(instrument, candidates);
                !near.empty()) {
                message += "; did you mean " + quoted(near) + "?";
            }
            errors.push_back({at + "/instrument", message});
        }
        const json::Array& bars = track.find("bars")->as_array();
        if (static_cast<int>(bars.size()) > length) {
            errors.push_back({at + "/bars", "has " + std::to_string(bars.size()) +
                                                " bars, more than the score's " +
                                                std::to_string(length)});
        }
        const int velocity = value_or(track, "velocity", kDefaultVelocity);
        for (std::size_t b = 0; b < bars.size(); ++b) {
            const BarNotes decoded =
                decode_bar(bars[b].as_string(), bar_beats, static_cast<double>(b) * bar_beats,
                           velocity, drums);
            if (!decoded.error.empty()) {
                errors.push_back({at + "/bars/" + std::to_string(b), decoded.error});
            }
        }
    }
    if (length == 0) {
        errors.push_back({"/tracks", "no track has any bars; give the score its length in "
                                     "\"bars\""});
    }
    if (const json::Value* sections = doc.find("sections"); sections != nullptr) {
        std::set<std::string_view> section_names;
        std::int64_t previous = -1;
        for (std::size_t i = 0; i < sections->size(); ++i) {
            const std::string at = "/sections/" + std::to_string(i);
            const json::Value& section = sections->as_array()[i];
            const std::string& name = section.find("name")->as_string();
            const std::int64_t bar = section.find("bar")->as_int();
            if (!section_names.insert(name).second) {
                errors.push_back({at + "/name", "another section is called " + quoted(name)});
            }
            if (bar >= length && length > 0) {
                errors.push_back(
                    {at + "/bar", "is past the score's last bar, " + std::to_string(length - 1)});
            } else if (bar <= previous) {
                errors.push_back({at + "/bar", "must come after the section before it, at bar " +
                                                   std::to_string(previous)});
            }
            previous = std::max(previous, bar);
        }
    }
    return errors;
}

Result<Score> parse_score(const json::Value& doc) {
    if (const auto errors = validate_score(doc); !errors.empty()) {
        const json::ValidationError& first = errors.front();
        return make_error(ErrorCode::InvalidArgument,
                          (first.path.empty() ? std::string("root") : first.path) + ": " +
                              first.message);
    }
    Score score;
    score.bpm = value_or(doc, "bpm", kDefaultBpm);
    score.bar_beats = value_or(doc, "bar_beats", kDefaultBarBeats);
    score.beat_unit = value_or(doc, "beat_unit", kDefaultBeatUnit);
    score.key = value_or(doc, "key", std::string());
    score.bars = score_length(doc);
    if (const json::Value* sections = doc.find("sections"); sections != nullptr) {
        for (const json::Value& section : sections->as_array()) {
            score.sections.push_back({section.find("name")->as_string(),
                                      static_cast<int>(section.find("bar")->as_int())});
        }
    }
    for (const json::Value& entry : doc.find("tracks")->as_array()) {
        Track track;
        track.name = entry.find("name")->as_string();
        track.instrument = value_or(entry, "instrument", std::string(kDefaultInstrument));
        track.drums = track.instrument == kDrumKit;
        track.program = track.drums ? 0 : instrument_program(track.instrument).value_or(0);
        track.volume_db = value_or(entry, "volume_db", 0.0);
        track.pan = value_or(entry, "pan", 0.0);
        track.velocity = value_or(entry, "velocity", kDefaultVelocity);
        const json::Array& bars = entry.find("bars")->as_array();
        for (std::size_t b = 0; b < bars.size(); ++b) {
            track.bars.push_back(bars[b].as_string());
            BarNotes decoded =
                decode_bar(track.bars.back(), score.bar_beats,
                           static_cast<double>(b) * score.bar_beats, track.velocity, track.drums);
            track.notes.insert(track.notes.end(), decoded.notes.begin(), decoded.notes.end());
        }
        score.tracks.push_back(std::move(track));
    }
    return score;
}

json::Value score_to_json(const Score& score) {
    json::Value doc;
    doc["format"] = 1;
    if (score.bpm != kDefaultBpm) {
        doc["bpm"] = whole_or_real(score.bpm);
    }
    if (score.bar_beats != kDefaultBarBeats) {
        doc["bar_beats"] = score.bar_beats;
    }
    if (score.beat_unit != kDefaultBeatUnit) {
        doc["beat_unit"] = score.beat_unit;
    }
    if (!score.key.empty()) {
        doc["key"] = score.key;
    }
    std::size_t longest = 0;
    for (const Track& track : score.tracks) {
        longest = std::max(longest, track.bars.size());
    }
    if (static_cast<std::size_t>(score.bars) != longest) {
        doc["bars"] = score.bars;
    }
    if (!score.sections.empty()) {
        json::Array sections;
        for (const Section& each : score.sections) {
            json::Value section;
            section["name"] = each.name;
            section["bar"] = each.bar;
            sections.push_back(std::move(section));
        }
        doc["sections"] = std::move(sections);
    }
    json::Array tracks;
    for (const Track& each : score.tracks) {
        json::Value track;
        track["name"] = each.name;
        if (each.instrument != kDefaultInstrument) {
            track["instrument"] = each.instrument;
        }
        if (each.volume_db != 0.0) {
            track["volume_db"] = whole_or_real(each.volume_db);
        }
        if (each.pan != 0.0) {
            track["pan"] = whole_or_real(each.pan);
        }
        if (each.velocity != kDefaultVelocity) {
            track["velocity"] = each.velocity;
        }
        json::Array bars;
        for (const std::string& bar : each.bars) {
            bars.emplace_back(bar);
        }
        track["bars"] = std::move(bars);
        tracks.push_back(std::move(track));
    }
    doc["tracks"] = std::move(tracks);
    return doc;
}

} // namespace slash::soundtrack
