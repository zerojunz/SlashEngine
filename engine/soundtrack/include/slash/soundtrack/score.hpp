#pragma once

#include "slash/core/json.hpp"
#include "slash/core/json_schema.hpp"
#include "slash/core/result.hpp"

#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

// Scores as text: a .score.json file is a piece of music an agent writes and changes exactly,
// one bar or one instrument at a time. A score has a tempo, a meter, a key and named sections;
// each track plays a General MIDI instrument (or its drums), and holds its notes as one line of
// text per bar:
//
//     "D3:1 F3 A3 D4@110 ; D2:4"
//
// Tokens play one after another from the start of the bar: a note (C4 is middle C, with # or b),
// a MIDI note number or, on a drums track, a drum's name, then ":" and its length in beats
// (1, 0.5, 3/2), which carries on to the tokens after it, and "@" and a velocity. Notes joined
// by "+" are a chord, "r" is a rest and ";" starts another voice from the start of the bar.
// A voice's last note may ring past the bar line.
namespace slash::soundtrack {

inline constexpr std::string_view kScoreSuffix = ".score.json";

// One note as it sounds: its start and length in beats from the start of the score.
struct Note {
    double beat = 0.0;
    double length = 1.0;
    int pitch = 60;     // MIDI note number: 60 is middle C (C4), 69 is A4 (440 Hz)
    int velocity = 100; // 1 to 127
};

struct Track {
    std::string name;
    // A General MIDI instrument's name, such as "acoustic_grand_piano", or "drums".
    std::string instrument = "acoustic_grand_piano";
    int program = 0; // the instrument's General MIDI program, 0 to 127; 0 for drums
    bool drums = false;
    double volume_db = 0.0;
    double pan = 0.0;   // -1 is left, 1 right
    int velocity = 100; // what notes play at unless they say
    std::vector<std::string> bars;
    std::vector<Note> notes; // every bar's, by start, then pitch
};

// A named part of the score, from its bar to the next section's, or the end.
struct Section {
    std::string name;
    int bar = 0; // counted from 0, as a track's bars are
};

struct Score {
    double bpm = 120.0;
    int bar_beats = 4; // beats in a bar
    int beat_unit = 4; // the note a beat is: 4 a quarter note, 8 an eighth
    std::string key;   // such as "D minor"; "" if the score doesn't say
    int bars = 0;      // its length
    std::vector<Section> sections;
    std::vector<Track> tracks;

    [[nodiscard]] double beats() const { return static_cast<double>(bars) * bar_beats; }
    [[nodiscard]] double seconds() const { return beats() * 60.0 / bpm; }
};

[[nodiscard]] json::Value score_schema();
// Every error in a .score.json document, each with the JSON Pointer of the value at fault; a
// bar's errors name the token too.
[[nodiscard]] std::vector<json::ValidationError> validate_score(const json::Value& doc);
[[nodiscard]] Result<Score> parse_score(const json::Value& doc);
// The score as a .score.json document in canonical form, with each track's bars as written and
// fields at their defaults left out.
[[nodiscard]] json::Value score_to_json(const Score& score);

// The notes of one bar's text, starting at beat start of the score, or why the text isn't
// valid.
struct BarNotes {
    std::vector<Note> notes;
    std::string error;
};
[[nodiscard]] BarNotes decode_bar(std::string_view text, int bar_beats, double start, int velocity,
                                  bool drums);
// A track's notes as bar text, the shortest that plays them: each note's start and length
// snapped to 1/grid of a beat, chords joined, voices only where notes overlap. Velocity is the
// track's; flats spells notes with b instead of #. Empty bars at the end are left out.
[[nodiscard]] std::vector<std::string> encode_bars(std::span<const Note> notes, int bar_beats,
                                                   int velocity, bool drums, bool flats, int grid);

// A note's name, MIDI number or, for drums, a drum's name, as a MIDI note number.
[[nodiscard]] std::optional<int> parse_pitch(std::string_view name, bool drums);
// A MIDI note's name, such as "C#4" or "Db4" with flats; a drum's name for drums, where it has
// one.
[[nodiscard]] std::string pitch_name(int pitch, bool flats, bool drums);
// General MIDI's instruments, by program: "acoustic_grand_piano" to "gunshot".
[[nodiscard]] std::span<const std::string_view> instrument_names();
[[nodiscard]] std::optional<int> instrument_program(std::string_view name);
// General MIDI's drums by name, from "acoustic_kick" (35) to "open_triangle" (81).
[[nodiscard]] std::span<const std::string_view> drum_names();
inline constexpr int kFirstDrum = 35;

// A key such as "F# major": its tonic's pitch class (0 is C), whether it is minor, and how many
// sharps (above 0) or flats (below 0) its signature has. nullopt if it isn't a key.
struct Key {
    int tonic = 0;
    bool minor = false;
    int sharps = 0;
};
[[nodiscard]] std::optional<Key> parse_key(std::string_view text);
// The key with that signature, as MIDI's key signature gives it: "C major" for 0 and false.
[[nodiscard]] std::string key_name(int sharps, bool minor);

} // namespace slash::soundtrack
