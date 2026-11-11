#pragma once

// NPCs' stories: questlines and conversations, which Godot leaves to scripts. A .quest.json file
// is a questline of stages, each with journal text and objectives over the story's facts, its
// rewards (effects on facts) and where it goes next; a .dialogue.json file is a conversation of
// lines, each said by someone, with choices whose conditions and effects read and change facts.
// The story's facts live in a QuestLog component (usually the player's) with the quests it has
// started; NPC brains read and change them as facts named story.<name>, so quests, conversations
// and NPCs move each other along. A Conversation component on the listener keeps where a
// conversation is. Text is translated through l10n, as Godot's tr.
//
// check_story finds quests that can't be finished and lines no conversation reaches, from what
// every file can set.

#include "slash/core/json.hpp"
#include "slash/core/json_schema.hpp"
#include "slash/core/result.hpp"
#include "slash/npc/npc.hpp"
#include "slash/world/world.hpp"

#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace slash::npc {

inline constexpr std::string_view kQuestSuffix = ".quest.json";
inline constexpr std::string_view kDialogueSuffix = ".dialogue.json";
// Dialogue facts whose names start with this are the speaking NPC's own (its NpcAgent's).
inline constexpr std::string_view kSelfPrefix = "self.";

// A condition or effect on a fact by name, as a quest or dialogue file writes it.
struct NamedCondition {
    std::string fact;
    Compare compare = Compare::Equal;
    int value = 0;

    friend bool operator==(const NamedCondition&, const NamedCondition&) = default;
};
struct NamedEffect {
    std::string fact;
    bool add = false;
    int value = 0;

    friend bool operator==(const NamedEffect&, const NamedEffect&) = default;
};

// Where to go next: the first branch whose conditions hold.
struct Branch {
    std::vector<NamedCondition> when;
    std::string to;

    friend bool operator==(const Branch&, const Branch&) = default;
};

// ---- Quests

struct Objective {
    std::string id;
    std::string text;
    std::vector<NamedCondition> when; // met once these hold, and stays met
    bool optional = false;

    friend bool operator==(const Objective&, const Objective&) = default;
};

struct Stage {
    std::string id;
    std::string journal; // what the journal says once the stage is reached
    std::vector<Objective> objectives;
    std::vector<NamedCondition> fail_when; // the quest fails when these hold (none: never)
    std::vector<NamedEffect> rewards;      // applied when the stage's objectives are met
    std::vector<Branch> next;              // empty: the next stage in the file, or the end
    enum class End : std::uint8_t { None, Success, Failure };
    End end = End::None; // the quest ends here, with this result, once the stage is reached

    friend bool operator==(const Stage&, const Stage&) = default;
};

// A .quest.json file.
struct Quest {
    std::string title;
    std::string summary;
    std::vector<NamedCondition> start_when; // a waiting quest starts once these hold
    std::vector<std::string> external;      // facts game code sets, for check_story
    std::vector<Stage> stages;              // the first is where it starts

    [[nodiscard]] const Stage* stage(std::string_view id) const;

    friend bool operator==(const Quest&, const Quest&) = default;
};

[[nodiscard]] json::Value quest_schema();
// Every error in a .quest.json document, each with the JSON Pointer of the bad value.
[[nodiscard]] std::vector<json::ValidationError> validate_quest(const json::Value& doc);
[[nodiscard]] Result<Quest> parse_quest(const json::Value& doc);

// ---- Dialogue

struct DialogueChoice {
    std::string text;
    std::vector<NamedCondition> when; // shown only while these hold
    std::vector<NamedEffect> effects;
    std::string start_quest;  // a .quest.json file to start, or ""
    std::vector<Branch> next; // empty: the conversation ends

    friend bool operator==(const DialogueChoice&, const DialogueChoice&) = default;
};

struct Line {
    std::string id;
    std::string speaker; // who says it, as shown; "" is the speaking entity
    std::string text;
    std::vector<NamedEffect> effects; // applied when the line is said
    std::string start_quest;
    std::vector<DialogueChoice> choices;
    std::vector<Branch> next; // with no choices; empty: the conversation ends

    friend bool operator==(const Line&, const Line&) = default;
};

// A .dialogue.json file.
struct Dialogue {
    std::vector<Branch> start;
    std::vector<Line> lines; // by id

    [[nodiscard]] const Line* line(std::string_view id) const;

    friend bool operator==(const Dialogue&, const Dialogue&) = default;
};

[[nodiscard]] json::Value dialogue_schema();
[[nodiscard]] std::vector<json::ValidationError> validate_dialogue(const json::Value& doc);
[[nodiscard]] Result<Dialogue> parse_dialogue(const json::Value& doc);

// ---- The story in a world

// The story's fact of that name in an entity's QuestLog: 0 when it has none.
[[nodiscard]] int story_fact(const world::World& world, entt::entity log, std::string_view fact);
// Sets a story fact in an entity's QuestLog, adding one if it has none, and moves its quests on.
void set_story_fact(world::World& world, entt::entity log, std::string_view fact, int value);
// The QuestLog an NPC's story.<name> facts read: its NpcAgent's story, else the first in the
// world's storage order; null when there is none.
[[nodiscard]] entt::entity story_log(const world::World& world, entt::entity npc);

// Starts a quest in an entity's QuestLog, adding one if it has none, and moves it on at once.
// Fails when the file doesn't load; a quest already started stays as it is.
Result<void> start_quest(world::World& world, entt::entity log, std::string_view path);
// Moves an entity's quests on as far as its facts take them now, as the quests system does each
// tick: starting waiting quests, meeting objectives, failing and finishing.
void update_quests(world::World& world, entt::entity log);

// What a quest looks like in a journal: its stage, its result and, translated, its title, what
// each stage reached says and the current stage's objectives.
struct QuestView {
    std::string quest;
    std::string title;
    std::string state; // "waiting", "active", "succeeded" or "failed"
    std::string stage;
    std::vector<std::string> journal;
    struct ObjectiveView {
        std::string id;
        std::string text;
        bool done = false;
        bool optional = false;
    };
    std::vector<ObjectiveView> objectives;
};
[[nodiscard]] Result<std::vector<QuestView>> quests_of(world::World& world, entt::entity log);

// Starts a conversation: the listener (who has the QuestLog its facts read) hears speaker (whose
// NpcAgent's facts are self.<name>) from the start of the dialogue at path, or from line. Says
// the first line.
Result<void> start_dialogue(world::World& world, entt::entity listener, entt::entity speaker,
                            std::string_view path, std::string_view line = {});
// Moves a conversation on: takes the choice, by its number among the choices shown, or goes on
// from a line without choices. Ends the conversation where nothing comes next.
Result<void> advance_dialogue(world::World& world, entt::entity listener,
                              std::optional<std::size_t> choice = std::nullopt);

// The line a listener is hearing, translated, with the choices shown; talking is false when the
// listener's conversation has ended or never began.
struct LineView {
    bool talking = false;
    std::string dialogue;
    std::string line;
    std::string speaker; // the speaker's name as shown
    std::string entity;  // the speaking entity's ID
    std::string text;
    std::vector<std::string> choices;
};
[[nodiscard]] Result<LineView> current_line(world::World& world, entt::entity listener);

// ---- Checks

// A problem in a story file.
struct StoryProblem {
    std::string file;
    std::string path; // JSON Pointer into the file
    std::string message;
    bool error = true; // false: a warning
};
// The files check_story reads, by project path: quests, dialogues and brains (those with story
// facts set them).
struct StoryFiles {
    std::map<std::string, json::Value, std::less<>> quests;
    std::map<std::string, json::Value, std::less<>> dialogues;
    std::map<std::string, json::Value, std::less<>> brains;
};
// Finds what can't happen, from every value any file sets each story fact to (dialogue effects,
// quest rewards, brains' story.<name> effects and quests' external facts): quests that can't be
// started or finished, stages and lines nothing reaches, objectives and choices whose conditions
// never hold, and quests a file starts that don't exist. Files that don't validate are reported
// as such. reader reads what brains extend.
[[nodiscard]] std::vector<StoryProblem> check_story(const StoryFiles& files,
                                                    const world::FileReader& reader = {});

} // namespace slash::npc
