#pragma once

#include "slash/core/json.hpp"

#include <string>
#include <vector>

// Drafting a game's cue sheet, the storyboard's cue entries: which music plays in each scene,
// story beat and game state.
namespace slash::soundtrack {

// A scene to plan music for: its project path and the storyboard IDs its entities' StoryRef
// components name.
struct PlanScene {
    std::string path;
    std::vector<std::string> story_ids;
};

struct Plan {
    // storyboard.edit's edits creating a cue for each scene, story beat and game state no cue
    // covers yet, each {"id", "set", "version": null}.
    json::Array edits;
    // The places, factions and characters those cues are about that no theme stands for, sorted.
    std::vector<std::string> unthemed;
};

// Drafts cues from the storyboard's entries and the project's scenes, sorted by path. A scene's
// cue plays the theme whose links name the scene, else the theme of the place it is set in, else
// of a faction, else of a character, found through its StoryRef IDs and what they name (a beat's
// place and characters, a character's home and faction, a place's region), and plays during the
// beats set there that no cue plays during. Each remaining beat gets a cue, found the same way,
// and so does each of states that no cue names.
[[nodiscard]] Plan plan_cues(const json::Object& entries, const std::vector<PlanScene>& scenes,
                             const std::vector<std::string>& states);

} // namespace slash::soundtrack
