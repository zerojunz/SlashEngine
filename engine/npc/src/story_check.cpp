// check_story: what can't happen in a game's quests and conversations, from every value any
// file sets each story fact to.

#include "facts.hpp"

#include "slash/npc/story.hpp"

#include <algorithm>
#include <deque>
#include <limits>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace slash::npc {

namespace {

// What some file can set a story fact to.
struct Setter {
    std::set<int> values; // set outright
    bool up = false;      // something adds to it
    bool down = false;    // something takes from it
    bool any = false;     // game code sets it (a quest's external facts)
};

using Setters = std::map<std::string, Setter, std::less<>>;

void note(Setters& setters, std::span<const NamedEffect> effects) {
    for (const NamedEffect& effect : effects) {
        if (effect.fact.starts_with(kSelfPrefix)) {
            continue;
        }
        Setter& setter = setters[effect.fact];
        if (!effect.add) {
            setter.values.insert(effect.value);
        } else if (effect.value > 0) {
            setter.up = true;
        } else if (effect.value < 0) {
            setter.down = true;
        }
    }
}

// Whether some value a fact can take meets a condition. A speaking NPC's own facts (self.<name>)
// count as able to.
bool can_meet(const Setters& setters, const NamedCondition& condition) {
    if (condition.fact.starts_with(kSelfPrefix)) {
        return true;
    }
    std::vector<int> candidates{0};
    const auto found = setters.find(condition.fact);
    if (found != setters.end()) {
        if (found->second.any || (found->second.up && found->second.down)) {
            return true;
        }
        candidates.insert(candidates.end(), found->second.values.begin(),
                          found->second.values.end());
    }
    const auto [low, high] = std::ranges::minmax(candidates);
    if (found != setters.end() && (found->second.up || found->second.down)) {
        for (const int near : {detail::added(condition.value, -1), condition.value,
                               detail::added(condition.value, 1)}) {
            if ((found->second.up && near >= low) || (found->second.down && near <= high)) {
                candidates.push_back(near);
            }
        }
    }
    return std::ranges::any_of(candidates, [&](int value) {
        return detail::meets(condition.compare, value, condition.value);
    });
}

bool can_hold(const Setters& setters, std::span<const NamedCondition> conditions) {
    return std::ranges::all_of(conditions,
                               [&](const NamedCondition& c) { return can_meet(setters, c); });
}

// Reports each condition of a list that no fact's values meet.
void report_never(const Setters& setters, std::span<const NamedCondition> conditions,
                  const std::string& file, const std::string& at, std::string_view what, bool error,
                  std::vector<StoryProblem>& out) {
    for (const NamedCondition& condition : conditions) {
        if (!can_meet(setters, condition)) {
            out.push_back({.file = file,
                           .path = at + "/" + json::pointer_escape(condition.fact),
                           .message = std::string(what) + " never holds: nothing sets \"" +
                                      condition.fact + "\" to a value that meets it",
                           .error = error});
        }
    }
}

void report_invalid(const std::string& file, const std::vector<json::ValidationError>& errors,
                    std::vector<StoryProblem>& out) {
    for (const json::ValidationError& error : errors) {
        out.push_back({.file = file, .path = error.path, .message = error.message, .error = true});
    }
}

void check_quest(const std::string& file, const Quest& quest, const Setters& setters,
                 std::vector<StoryProblem>& out) {
    report_never(setters, quest.start_when, file, "/start_when", "start_when", true, out);
    const std::size_t count = quest.stages.size();
    const auto index_of = [&](std::string_view id) {
        return static_cast<std::size_t>(std::ranges::find(quest.stages, id, &Stage::id) -
                                        quest.stages.begin());
    };
    const auto passable = [&](const Stage& stage) {
        return std::ranges::all_of(stage.objectives, [&](const Objective& objective) {
            return objective.optional || can_hold(setters, objective.when);
        });
    };
    std::vector<bool> reached(count, false);
    bool finishes = false;
    std::deque<std::size_t> open{0};
    reached[0] = true;
    while (!open.empty()) {
        const std::size_t at = open.front();
        open.pop_front();
        const Stage& stage = quest.stages[at];
        if (stage.end != Stage::End::None) {
            finishes = true;
            continue;
        }
        if (!passable(stage)) {
            continue;
        }
        const auto visit = [&](std::size_t next) {
            if (next < count && !reached[next]) {
                reached[next] = true;
                open.push_back(next);
            }
        };
        if (stage.next.empty()) {
            if (at + 1 == count) {
                finishes = true;
            } else {
                visit(at + 1);
            }
            continue;
        }
        for (const Branch& branch : stage.next) {
            if (can_hold(setters, branch.when)) {
                visit(index_of(branch.to));
            }
        }
    }
    for (std::size_t s = 0; s < count; ++s) {
        const std::string at = "/stages/" + std::to_string(s);
        if (!reached[s]) {
            out.push_back(
                {.file = file,
                 .path = at,
                 .message = "no way through the quest reaches stage \"" + quest.stages[s].id + "\"",
                 .error = true});
            continue;
        }
        const std::vector<Objective>& objectives = quest.stages[s].objectives;
        for (std::size_t o = 0; o < objectives.size(); ++o) {
            report_never(setters, objectives[o].when, file,
                         at + "/objectives/" + std::to_string(o) + "/when",
                         "objective \"" + objectives[o].id + "\"", !objectives[o].optional, out);
        }
        const std::vector<Branch>& next = quest.stages[s].next;
        for (std::size_t b = 0; b < next.size(); ++b) {
            report_never(setters, next[b].when, file, at + "/next/" + std::to_string(b) + "/when",
                         "this branch", false, out);
        }
    }
    if (!finishes) {
        out.push_back({.file = file,
                       .path = "",
                       .message = "the quest can't be finished: no way through its stages "
                                  "reaches an end stage or the last stage's end",
                       .error = true});
    }
}

void check_dialogue(const std::string& file, const Dialogue& dialogue, const Setters& setters,
                    const StoryFiles& files, std::vector<StoryProblem>& out) {
    std::set<std::string, std::less<>> reached;
    std::deque<std::string> open;
    const auto visit = [&](std::span<const Branch> branches) {
        for (const Branch& branch : branches) {
            if (can_hold(setters, branch.when) && reached.insert(branch.to).second) {
                open.push_back(branch.to);
            }
        }
    };
    visit(dialogue.start);
    while (!open.empty()) {
        const Line* line = dialogue.line(open.front());
        open.pop_front();
        if (line == nullptr) {
            continue;
        }
        if (line->choices.empty()) {
            visit(line->next);
            continue;
        }
        for (const DialogueChoice& choice : line->choices) {
            if (can_hold(setters, choice.when)) {
                visit(choice.next);
            }
        }
    }
    const auto check_quest_path = [&](const std::string& path, const std::string& at) {
        if (!path.empty() && !files.quests.contains(path)) {
            out.push_back({.file = file,
                           .path = at,
                           .message = "no quest file \"" + path + "\" in the project",
                           .error = true});
        }
    };
    for (const Line& line : dialogue.lines) {
        const std::string at = "/lines/" + json::pointer_escape(line.id);
        if (!reached.contains(line.id)) {
            out.push_back(
                {.file = file,
                 .path = at,
                 .message = "no way through the conversation reaches line \"" + line.id + "\"",
                 .error = true});
        }
        check_quest_path(line.start_quest, at + "/start_quest");
        for (std::size_t c = 0; c < line.choices.size(); ++c) {
            const std::string where = at + "/choices/" + std::to_string(c);
            report_never(setters, line.choices[c].when, file, where + "/when", "this choice's when",
                         true, out);
            check_quest_path(line.choices[c].start_quest, where + "/start_quest");
        }
    }
}

} // namespace

std::vector<StoryProblem> check_story(const StoryFiles& files, const world::FileReader& reader) {
    std::vector<StoryProblem> out;
    Setters setters;
    std::map<std::string, Quest, std::less<>> quests;
    std::map<std::string, Dialogue, std::less<>> dialogues;
    for (const auto& [path, doc] : files.quests) {
        if (const auto errors = validate_quest(doc); !errors.empty()) {
            report_invalid(path, errors, out);
            continue;
        }
        Quest quest = parse_quest(doc).value();
        for (const Stage& stage : quest.stages) {
            note(setters, stage.rewards);
        }
        for (const std::string& fact : quest.external) {
            setters[fact].any = true;
        }
        quests.emplace(path, std::move(quest));
    }
    for (const auto& [path, doc] : files.dialogues) {
        if (const auto errors = validate_dialogue(doc); !errors.empty()) {
            report_invalid(path, errors, out);
            continue;
        }
        Dialogue dialogue = parse_dialogue(doc).value();
        for (const Line& line : dialogue.lines) {
            note(setters, line.effects);
            for (const DialogueChoice& choice : line.choices) {
                note(setters, choice.effects);
            }
        }
        dialogues.emplace(path, std::move(dialogue));
    }
    for (const auto& [path, doc] : files.brains) {
        if (const auto errors = validate_brain(doc, reader); !errors.empty()) {
            report_invalid(path, errors, out);
            continue;
        }
        const Brain brain = parse_brain(doc, reader).value();
        for (const Action& action : brain.actions) {
            for (const Effect& effect : action.effects) {
                if (std::ranges::find(brain.story, effect.fact) == brain.story.end()) {
                    continue;
                }
                const NamedEffect named{.fact =
                                            brain.facts[effect.fact].substr(kStoryPrefix.size()),
                                        .add = effect.add,
                                        .value = effect.value};
                note(setters, std::span(&named, 1));
            }
        }
    }
    for (const auto& [path, quest] : quests) {
        check_quest(path, quest, setters, out);
    }
    for (const auto& [path, dialogue] : dialogues) {
        check_dialogue(path, dialogue, setters, files, out);
    }
    return out;
}

} // namespace slash::npc
