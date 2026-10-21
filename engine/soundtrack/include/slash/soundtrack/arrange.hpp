#pragma once

#include "slash/core/json.hpp"

#include <functional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// Arranging the storyboard's cue sheet into interactive music: one .audio.json file the runtime
// plays, its clips switched by a player's switch_to_clip parameter.
namespace slash::soundtrack {

inline constexpr std::string_view kSoundtrackPath = "music/soundtrack.audio.json";

struct ArrangeOptions {
    // The interactive stream to write; its layer files go in the folder of the same name.
    std::string out{kSoundtrackPath};
    // Only these cues, in this order. Default: every cue, by ID.
    std::vector<std::string> cues{}; // NOLINT(readability-redundant-member-init)
    // The cue it starts with. Default: the first.
    std::string initial{}; // NOLINT(readability-redundant-member-init)
};

// Something wrong with a cue: missing is a music file the cue names that can't play (it doesn't
// exist, isn't rendered or isn't music) or a layer with no file, rather than music not written
// yet or a sting that has nothing to follow.
struct ArrangeProblem {
    std::string cue;
    std::string message;
    bool missing = false;
};

struct Arranged {
    json::Value music; // the interactive stream
    // The synchronized streams the clips of cues with several files or layers play, by path.
    std::vector<std::pair<std::string, json::Value>> files;
    // What each clip is: {"name", "cue", "layer"?, "stream"}.
    json::Array clips;
    std::vector<ArrangeProblem> problems;
};

// Turns cues into the clips of an interactive stream. A cue plays its music: the .audio.json,
// .ogg, .wav or .mp3 files it names, or what soundtrack.render makes of a .score.json file.
// - A cue with layers becomes a clip per layer, each playing that layer's file and those below
//   it together, as a synchronized stream; switching layers keeps the music's place and
//   crossfades on the next beat.
// - A cue that plays once is a sting: it plays between leaving the cues in its after and going
//   on, as a filler clip, or else it plays when asked for and then goes back to the clip it
//   interrupted, where that clip was.
// - Every other switch waits for the next bar.
// A cue with no music, or whose files don't exist (exists says), is a problem; one with no music
// is left out.
[[nodiscard]] Arranged arrange(const json::Object& entries, const ArrangeOptions& options,
                               const std::function<bool(std::string_view path)>& exists);

} // namespace slash::soundtrack
