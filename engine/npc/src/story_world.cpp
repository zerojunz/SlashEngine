// Quests and conversations in a world: QuestLog components move their quests on as the story's
// facts change, and Conversation components keep where a conversation is.

#include "facts.hpp"

#include "slash/l10n/l10n.hpp"
#include "slash/npc/story.hpp"
#include "slash/world/components.hpp"

#include <algorithm>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace slash::npc {

namespace {

// The most stages a quest moves through in one update, so a loop of stages that all go on at
// once ends.
constexpr int kMaxSteps = 64;

int value_in(const std::map<std::string, int>& facts, std::string_view name) {
    const auto found = facts.find(std::string(name));
    return found == facts.end() ? 0 : found->second;
}

bool holds_in(std::span<const NamedCondition> conditions, const std::map<std::string, int>& facts) {
    return std::ranges::all_of(conditions, [&](const NamedCondition& c) {
        return detail::meets(c.compare, value_in(facts, c.fact), c.value);
    });
}

void apply_in(std::span<const NamedEffect> effects, std::map<std::string, int>& facts) {
    for (const NamedEffect& effect : effects) {
        const int value =
            effect.add ? detail::added(value_in(facts, effect.fact), effect.value) : effect.value;
        if (value == 0) {
            facts.erase(effect.fact);
        } else {
            facts[effect.fact] = value;
        }
    }
}

template <typename T>
Result<std::shared_ptr<const T>> load(world::World& world, std::string_view path) {
    asset::Assets* assets = detail::assets_of(world);
    if (assets == nullptr) {
        return Error{ErrorCode::InvalidArgument, "the world has no npc system"};
    }
    auto loaded = assets->load<T>(path);
    if (!loaded) {
        return Error{loaded.error().code, std::string(path) + ": " + loaded.error().message};
    }
    return loaded.value().share();
}

json::Value quest_data(std::string_view quest) {
    json::Value data;
    data["quest"] = std::string(quest);
    return data;
}

// Puts a quest at a stage, finishing it if the stage is an end.
void enter_stage(world::World& world, std::string_view id, const std::string& path,
                 const Quest& quest, const Stage& stage, world::QuestProgress& progress) {
    json::Value data = quest_data(path);
    data["stage"] = stage.id;
    data["previous"] = progress.stage;
    progress.stage = stage.id;
    progress.reached.push_back(stage.id);
    progress.done.clear();
    world.emit("quest_stage_changed", id, std::move(data));
    (void)quest;
    if (stage.end != Stage::End::None) {
        const bool success = stage.end == Stage::End::Success;
        progress.state = success ? "succeeded" : "failed";
        json::Value finished = quest_data(path);
        finished["result"] = progress.state;
        world.emit("quest_finished", id, std::move(finished));
    }
}

void finish(world::World& world, std::string_view id, const std::string& path,
            world::QuestProgress& progress, bool success) {
    progress.state = success ? "succeeded" : "failed";
    json::Value data = quest_data(path);
    data["result"] = progress.state;
    world.emit("quest_finished", id, std::move(data));
}

// One move of an active quest; true when anything changed.
bool step_quest(world::World& world, std::string_view id, const std::string& path,
                const Quest& quest, world::QuestLog& log, world::QuestProgress& progress) {
    const Stage* stage = quest.stage(progress.stage);
    if (stage == nullptr) {
        // The file changed under it: start again from the first stage.
        enter_stage(world, id, path, quest, quest.stages.front(), progress);
        return true;
    }
    if (!stage->fail_when.empty() && holds_in(stage->fail_when, log.facts)) {
        finish(world, id, path, progress, false);
        return true;
    }
    bool changed = false;
    bool all = true;
    for (const Objective& objective : stage->objectives) {
        const bool met = std::ranges::find(progress.done, objective.id) != progress.done.end();
        if (!met && holds_in(objective.when, log.facts)) {
            progress.done.push_back(objective.id);
            json::Value data = quest_data(path);
            data["stage"] = stage->id;
            data["objective"] = objective.id;
            world.emit("quest_objective_done", id, std::move(data));
            changed = true;
        } else if (!met && !objective.optional) {
            all = false;
        }
    }
    if (!all) {
        return changed;
    }
    // Where it goes, with the rewards given: they apply only when it moves on.
    std::map<std::string, int> after = log.facts;
    apply_in(stage->rewards, after);
    const Stage* next = nullptr;
    if (stage->next.empty()) {
        const auto at = std::ranges::find(quest.stages, stage->id, &Stage::id);
        if (at + 1 == quest.stages.end()) {
            log.facts = std::move(after);
            finish(world, id, path, progress, true);
            return true;
        }
        next = &*(at + 1);
    } else {
        for (const Branch& branch : stage->next) {
            if (holds_in(branch.when, after)) {
                next = quest.stage(branch.to);
                break;
            }
        }
        if (next == nullptr) {
            return changed; // waits for a branch to hold
        }
    }
    log.facts = std::move(after);
    enter_stage(world, id, path, quest, *next, progress);
    return true;
}

world::Conversation* conversation_of(world::World& world, entt::entity listener) {
    if (!world.valid(listener)) {
        return nullptr;
    }
    auto* conversation = world.ecs().try_get<world::Conversation>(listener);
    return conversation != nullptr && !conversation->dialogue.empty() ? conversation : nullptr;
}

// A dialogue fact: the speaker's own for self.<name>, else the listener's story fact.
int dialogue_fact(world::World& world, entt::entity listener, entt::entity speaker,
                  std::string_view name) {
    if (name.starts_with(kSelfPrefix)) {
        const std::shared_ptr<const Brain> brain =
            world.valid(speaker) ? brain_of(world, speaker) : nullptr;
        if (brain == nullptr) {
            return 0;
        }
        const std::size_t index = brain->fact(name.substr(kSelfPrefix.size()));
        const std::vector<int> facts = facts_of(world, speaker);
        return index < facts.size() ? facts[index] : 0;
    }
    return story_fact(world, listener, name);
}

bool dialogue_holds(world::World& world, entt::entity listener, entt::entity speaker,
                    std::span<const NamedCondition> conditions) {
    return std::ranges::all_of(conditions, [&](const NamedCondition& c) {
        return detail::meets(c.compare, dialogue_fact(world, listener, speaker, c.fact), c.value);
    });
}

void dialogue_effects(world::World& world, entt::entity listener, entt::entity speaker,
                      std::span<const NamedEffect> effects) {
    for (const NamedEffect& effect : effects) {
        const int value =
            effect.add
                ? detail::added(dialogue_fact(world, listener, speaker, effect.fact), effect.value)
                : effect.value;
        if (effect.fact.starts_with(kSelfPrefix)) {
            if (world.valid(speaker)) {
                (void)set_fact(world, speaker,
                               std::string_view(effect.fact).substr(kSelfPrefix.size()), value);
            }
        } else {
            set_story_fact(world, listener, effect.fact, value);
        }
    }
}

const Branch* first_holding(world::World& world, entt::entity listener, entt::entity speaker,
                            std::span<const Branch> branches) {
    for (const Branch& branch : branches) {
        if (dialogue_holds(world, listener, speaker, branch.when)) {
            return &branch;
        }
    }
    return nullptr;
}

// Says a line: its effects, the quest it starts and its event.
void say(world::World& world, entt::entity listener, const Dialogue& dialogue,
         world::Conversation& conversation, const Line& line) {
    conversation.line = line.id;
    conversation.said.push_back(line.id);
    const entt::entity speaker = conversation.speaker;
    const std::string path = conversation.dialogue;
    dialogue_effects(world, listener, speaker, line.effects);
    if (!line.start_quest.empty()) {
        (void)start_quest(world, listener, line.start_quest);
    }
    json::Value data;
    data["dialogue"] = path;
    data["line"] = line.id;
    data["speaker"] = world.valid(speaker) ? std::string(world.id_of(speaker)) : std::string();
    world.emit("dialogue_line", world.id_of(listener), std::move(data));
    (void)dialogue;
}

void end_conversation(world::World& world, entt::entity listener) {
    auto& conversation = world.ecs().get<world::Conversation>(listener);
    json::Value data;
    data["dialogue"] = conversation.dialogue;
    data["line"] = conversation.line;
    conversation = world::Conversation{};
    world.emit("dialogue_ended", world.id_of(listener), std::move(data));
}

} // namespace

int story_fact(const world::World& world, entt::entity log, std::string_view fact) {
    if (!world.valid(log)) {
        return 0;
    }
    const auto* quests = world.ecs().try_get<world::QuestLog>(log);
    return quests == nullptr ? 0 : value_in(quests->facts, fact);
}

void set_story_fact(world::World& world, entt::entity log, std::string_view fact, int value) {
    if (!world.valid(log)) {
        return;
    }
    auto& quests = world.ecs().get_or_emplace<world::QuestLog>(log);
    if (value == 0) {
        quests.facts.erase(std::string(fact));
    } else {
        quests.facts[std::string(fact)] = value;
    }
    update_quests(world, log);
}

entt::entity story_log(const world::World& world, entt::entity npc) {
    if (world.valid(npc)) {
        if (const auto* agent = world.ecs().try_get<world::NpcAgent>(npc);
            agent != nullptr && agent->story != entt::null && world.valid(agent->story) &&
            world.ecs().all_of<world::QuestLog>(agent->story)) {
            return agent->story;
        }
    }
    // Views run from the newest entity to the oldest; the last seen was made first.
    entt::entity first = entt::null;
    for (const entt::entity entity : world.ecs().view<world::QuestLog>()) {
        first = entity;
    }
    return first;
}

Result<void> start_quest(world::World& world, entt::entity log, std::string_view path) {
    if (!world.valid(log)) {
        return Error{ErrorCode::InvalidArgument, "no such entity"};
    }
    auto quest = load<Quest>(world, path);
    if (!quest) {
        return quest.error();
    }
    auto& quests = world.ecs().get_or_emplace<world::QuestLog>(log);
    const auto [at, added] = quests.quests.try_emplace(std::string(path));
    if (!added && at->second.state != "waiting") {
        return {};
    }
    at->second.state = "active";
    world.emit("quest_started", world.id_of(log), quest_data(path));
    enter_stage(world, world.id_of(log), at->first, *quest.value(), quest.value()->stages.front(),
                at->second);
    update_quests(world, log);
    return {};
}

void update_quests(world::World& world, entt::entity log) {
    if (!world.valid(log)) {
        return;
    }
    auto* quests = world.ecs().try_get<world::QuestLog>(log);
    if (quests == nullptr || !quests->enabled) {
        return;
    }
    const std::string id(world.id_of(log));
    for (int step = 0; step < kMaxSteps; ++step) {
        bool changed = false;
        for (auto& [path, progress] : quests->quests) {
            auto quest = load<Quest>(world, path);
            if (!quest) {
                continue;
            }
            const Quest& file = *quest.value();
            if (progress.state == "waiting" && holds_in(file.start_when, quests->facts)) {
                progress.state = "active";
                world.emit("quest_started", id, quest_data(path));
                enter_stage(world, id, path, file, file.stages.front(), progress);
                changed = true;
            }
            if (progress.state == "active" &&
                step_quest(world, id, path, file, *quests, progress)) {
                changed = true;
            }
        }
        if (!changed) {
            return;
        }
    }
}

Result<std::vector<QuestView>> quests_of(world::World& world, entt::entity log) {
    std::vector<QuestView> out;
    if (!world.valid(log)) {
        return Error{ErrorCode::InvalidArgument, "no such entity"};
    }
    const auto* quests = world.ecs().try_get<world::QuestLog>(log);
    if (quests == nullptr) {
        return out;
    }
    for (const auto& [path, progress] : quests->quests) {
        QuestView view;
        view.quest = path;
        view.state = progress.state;
        view.stage = progress.stage;
        auto quest = load<Quest>(world, path);
        if (!quest) {
            out.push_back(std::move(view));
            continue;
        }
        const Quest& file = *quest.value();
        view.title = l10n::tr(world, file.title);
        for (const std::string& reached : progress.reached) {
            if (const Stage* stage = file.stage(reached);
                stage != nullptr && !stage->journal.empty()) {
                view.journal.push_back(l10n::tr(world, stage->journal));
            }
        }
        if (const Stage* stage = file.stage(progress.stage);
            stage != nullptr && progress.state == "active") {
            for (const Objective& objective : stage->objectives) {
                view.objectives.push_back(
                    {.id = objective.id,
                     .text = l10n::tr(world, objective.text),
                     .done = std::ranges::find(progress.done, objective.id) != progress.done.end(),
                     .optional = objective.optional});
            }
        }
        out.push_back(std::move(view));
    }
    return out;
}

Result<void> start_dialogue(world::World& world, entt::entity listener, entt::entity speaker,
                            std::string_view path, std::string_view line) {
    if (!world.valid(listener)) {
        return Error{ErrorCode::InvalidArgument, "no such listener"};
    }
    auto dialogue = load<Dialogue>(world, path);
    if (!dialogue) {
        return dialogue.error();
    }
    const Dialogue& file = *dialogue.value();
    const Line* first = nullptr;
    if (!line.empty()) {
        first = file.line(line);
        if (first == nullptr) {
            return Error{ErrorCode::NotFound,
                         "the dialogue has no line \"" + std::string(line) + "\""};
        }
    } else {
        const Branch* branch = first_holding(world, listener, speaker, file.start);
        if (branch == nullptr) {
            return Error{ErrorCode::InvalidArgument, "none of the dialogue's start branches hold"};
        }
        first = file.line(branch->to);
    }
    auto& conversation = world.ecs().emplace_or_replace<world::Conversation>(listener);
    conversation.dialogue = std::string(path);
    conversation.speaker = speaker;
    say(world, listener, file, conversation, *first);
    return {};
}

Result<void> advance_dialogue(world::World& world, entt::entity listener,
                              std::optional<std::size_t> choice) {
    world::Conversation* conversation = conversation_of(world, listener);
    if (conversation == nullptr) {
        return Error{ErrorCode::InvalidArgument, "the entity isn't in a conversation"};
    }
    auto dialogue = load<Dialogue>(world, conversation->dialogue);
    if (!dialogue) {
        return dialogue.error();
    }
    const Dialogue& file = *dialogue.value();
    const Line* line = file.line(conversation->line);
    if (line == nullptr) {
        end_conversation(world, listener);
        return {};
    }
    const entt::entity speaker = conversation->speaker;
    std::span<const Branch> next = line->next;
    if (!line->choices.empty()) {
        std::vector<const DialogueChoice*> shown;
        for (const DialogueChoice& option : line->choices) {
            if (dialogue_holds(world, listener, speaker, option.when)) {
                shown.push_back(&option);
            }
        }
        if (!choice || *choice >= shown.size()) {
            return Error{ErrorCode::InvalidArgument, "choose one of the " +
                                                         std::to_string(shown.size()) +
                                                         " choices shown, from 0"};
        }
        const DialogueChoice& chosen = *shown[*choice];
        dialogue_effects(world, listener, speaker, chosen.effects);
        if (!chosen.start_quest.empty()) {
            (void)start_quest(world, listener, chosen.start_quest);
        }
        next = chosen.next;
    } else if (choice) {
        return Error{ErrorCode::InvalidArgument, "the line has no choices"};
    }
    const Branch* branch = first_holding(world, listener, speaker, next);
    const Line* following = branch != nullptr ? file.line(branch->to) : nullptr;
    if (following == nullptr) {
        end_conversation(world, listener);
        return {};
    }
    say(world, listener, file, world.ecs().get<world::Conversation>(listener), *following);
    return {};
}

Result<LineView> current_line(world::World& world, entt::entity listener) {
    LineView view;
    const world::Conversation* conversation = conversation_of(world, listener);
    if (conversation == nullptr) {
        return view;
    }
    auto dialogue = load<Dialogue>(world, conversation->dialogue);
    if (!dialogue) {
        return dialogue.error();
    }
    const Line* line = dialogue.value()->line(conversation->line);
    if (line == nullptr) {
        return view;
    }
    const entt::entity speaker = conversation->speaker;
    view.talking = true;
    view.dialogue = conversation->dialogue;
    view.line = line->id;
    view.entity = world.valid(speaker) ? std::string(world.id_of(speaker)) : std::string();
    view.speaker = line->speaker.empty() ? view.entity : l10n::tr(world, line->speaker);
    view.text = l10n::tr(world, line->text);
    for (const DialogueChoice& option : line->choices) {
        if (dialogue_holds(world, listener, speaker, option.when)) {
            view.choices.push_back(l10n::tr(world, option.text));
        }
    }
    return view;
}

} // namespace slash::npc
