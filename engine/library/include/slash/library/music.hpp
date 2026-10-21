#pragma once

#include "slash/core/json.hpp"
#include "slash/core/result.hpp"
#include "slash/library/library.hpp"

#include <optional>
#include <string>
#include <string_view>
#include <vector>

// Music in the library: its tempo, length in beats and key, measured from the sound and as whoever
// made it said, so agents find a theme by its tempo and key and games time transitions by its
// beats.
namespace slash::library {

struct Key {
    int tonic = 0; // the tonic's pitch class: 0 for C, 1 for C# and so on
    bool minor = false;
};

// A key such as "D minor", "f# major", "Bb minor" or "Am", or nullopt for anything else.
[[nodiscard]] std::optional<Key> parse_key(std::string_view text);
// The key as audio::analyze names it, such as "Eb major" or "F# minor".
[[nodiscard]] std::string key_name(Key key);

// A tempo as JSON: 120 rather than 120.0 when it is whole.
[[nodiscard]] json::Value bpm_json(double bpm);

// What the author of a piece of music, or the request that made it, says it is: 0 or "" where
// they say nothing.
struct MusicFacts {
    double bpm = 0.0;
    int beat_count = 0;
    int bar_beats = 0;
    std::string key{}; // NOLINT(readability-redundant-member-init)
};

// Makes the record music and fills in its tempo, beats and key. An author's own track (trusted)
// keeps the facts its author gave; music a generator made keeps what its sound measures, the facts
// asked for only where it measures none, or where its tempo is twice or half theirs. A tempo
// whose beats nearly fill the sound is set so whole beats fill it, so the music loops on the beat.
// Returns where what was given and what the sound measures disagree, a sentence each.
[[nodiscard]] Result<std::vector<std::string>> describe_music(std::string_view data,
                                                              std::string_view path,
                                                              const MusicFacts& given, bool trusted,
                                                              Record& record);

} // namespace slash::library
