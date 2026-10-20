#pragma once

#include "slash/core/result.hpp"
#include "slash/soundtrack/score.hpp"

#include <string>
#include <string_view>
#include <vector>

// Scores to and from Standard MIDI Files, so music moves between the engine and any music
// program.
namespace slash::soundtrack {

inline constexpr int kMidiTicksPerQuarter = 480;
// Divisions of a beat that imported notes snap to: thirty-second notes and sixteenth-note
// triplets.
inline constexpr int kDefaultGrid = 24;

// A score as a Standard MIDI File (format 1): a first track with the tempo, meter, key and a
// marker per section, then a track per score track, drums on channel 10. Fails for a score with
// more tracks than MIDI has channels for.
[[nodiscard]] Result<std::string> write_midi(const Score& score);

struct MidiImport {
    Score score;
    int notes = 0;
    // What the score can't hold, such as tempo changes or pitch bends, and what moved to fit it.
    std::vector<std::string> problems;
};

// A Standard MIDI File (format 0 or 1) as a score: a track per MIDI track and channel that plays
// notes, named by the track's name, with its instrument from the channel's first program change;
// the first tempo, time signature and key signature; and a section per marker. Notes snap to
// 1/grid of a beat.
[[nodiscard]] Result<MidiImport> read_midi(std::string_view data, int grid = kDefaultGrid);

} // namespace slash::soundtrack
