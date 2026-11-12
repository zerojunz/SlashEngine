// NPCs with stories of their own: brains that extend a shared one, daily schedules and routes,
// .quest.json questlines, .dialogue.json conversations, story facts NPCs share with them, the
// check that every quest finishes and every line is reached, and the quest.* and dialogue.*
// commands.

#include "slash/asset/assets.hpp"
#include "slash/command/builtin.hpp"
#include "slash/command/command.hpp"
#include "slash/command/session.hpp"
#include "slash/core/json.hpp"
#include "slash/navigation/navigation.hpp"
#include "slash/npc/npc.hpp"
#include "slash/npc/story.hpp"
#include "slash/platform/file.hpp"
#include "slash/world/component_registry.hpp"
#include "slash/world/components.hpp"
#include "slash/world/world.hpp"

#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <filesystem>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace asset = slash::asset;
namespace command = slash::command;
namespace json = slash::json;
namespace npc = slash::npc;
namespace world = slash::world;

namespace {

constexpr double kTick = 1.0 / 60.0;

json::Value parse(std::string_view text) {
    auto parsed = json::parse(text);
    REQUIRE_MESSAGE(parsed.ok(), text);
    return std::move(parsed).value();
}

struct MemoryFiles {
    std::map<std::string, std::string, std::less<>> files;

    world::FileReader reader() {
        return [this](std::string_view path) -> slash::Result<std::string> {
            const auto it = files.find(path);
            if (it == files.end()) {
                return slash::make_error(slash::ErrorCode::NotFound, "no such file");
            }
            return it->second;
        };
    }

    asset::FileSource source() {
        return {
            .read = reader(),
            .stamp = [this](std::string_view path) -> std::optional<asset::FileStamp> {
                const auto it = files.find(path);
                if (it == files.end()) {
                    return std::nullopt;
                }
                return asset::FileStamp{.modified = 1, .size = it->second.size()};
            },
        };
    }
};

using Names = std::vector<std::string>;

// A world with the npc systems over files in memory, and the story events it emits.
struct Scene {
    MemoryFiles files;
    std::unique_ptr<asset::Assets> assets;
    world::ComponentRegistry components;
    world::World world{components};
    std::vector<world::Event> events;

    explicit Scene(std::map<std::string, std::string, std::less<>> given,
                   npc::Settings settings = {}) {
        files.files = std::move(given);
        assets = std::make_unique<asset::Assets>(files.source());
        world::register_builtin_components(components);
        slash::navigation::install(world, *assets);
        npc::install(world, *assets, settings);
        world.set_event_listener([this](const world::Event& event) { events.push_back(event); });
    }

    entt::entity create(std::string_view id, std::string_view components_json) {
        const entt::entity entity = world.create(id).value();
        const json::Value components_value = parse(components_json);
        for (const auto& [name, fields] : components_value.as_object()) {
            std::string path;
            const auto assigned = components.find(name)->assign(world, entity, fields, path);
            REQUIRE_MESSAGE(assigned.ok(), name << path << ": "
                                                << (assigned.ok() ? "" : assigned.error().message));
        }
        return entity;
    }

    void run(double seconds) {
        for (double t = 0.0; t < seconds - 1e-9; t += kTick) {
            world.step(kTick);
        }
    }

    // The story events seen, as "kind detail".
    Names story() const {
        Names out;
        for (const world::Event& event : events) {
            const bool quest = event.kind.starts_with("quest_");
            const bool dialogue = event.kind.starts_with("dialogue_");
            if (!quest && !dialogue) {
                continue;
            }
            std::string line = event.kind;
            for (const std::string_view key : {"stage", "objective", "result", "line"}) {
                if (const json::Value* value = event.data.find(key); value != nullptr) {
                    line += " " + value->as_string();
                }
            }
            out.push_back(std::move(line));
        }
        return out;
    }

    const world::QuestLog& log(entt::entity entity) {
        return world.ecs().get<world::QuestLog>(entity);
    }
};

slash::Vec3 place_of(Scene& scene, entt::entity entity) {
    if (const auto* t3 = scene.world.ecs().try_get<world::Transform3D>(entity); t3 != nullptr) {
        return t3->position;
    }
    const slash::Vec2 p = scene.world.ecs().get<world::Transform2D>(entity).position;
    return {p.x, p.y, 0.0F};
}

bool near(slash::Vec3 a, slash::Vec3 b, double within = 0.6) {
    const slash::Vec3 d = a - b;
    return std::sqrt(static_cast<double>((d.x * d.x) + (d.y * d.y) + (d.z * d.z))) <= within;
}

// A village's shared brain, and a guard's that extends it.
constexpr std::string_view kVillager = R"({"format": 1,
    "facts": {"tired": false, "fed": false},
    "goals": [{"name": "rest", "priority": 1, "when": {"tired": true}, "want": {"tired": false}},
              {"name": "eat", "priority": 2, "want": {"fed": true}}],
    "actions": [{"name": "sleep", "effects": {"tired": false}},
                {"name": "eat", "effects": {"fed": true}},
                {"name": "beg", "cost": 3, "effects": {"fed": true}}]})";

constexpr std::string_view kGuard = R"({"format": 1, "extends": "villager.npc.json",
    "traits": {"brave": 3},
    "facts": {"alert": false},
    "goals": [{"name": "rest", "priority": 5},
              {"name": "watch", "priority": 3, "when": {"brave": ">=2"}, "want": {"alert": true}}],
    "actions": [{"name": "beg", "remove": true},
                {"name": "look", "effects": {"alert": true}}]})";

// A watchman who walks his round from 8 to 20 o'clock and goes to bed after.
constexpr std::string_view kWatchman = R"({"format": 1,
    "facts": {"hour": 0, "patrolled": false},
    "routes": {"round": {"points": [[10, 0], [10, 10]], "closed": true}},
    "goals": [{"name": "patrol", "when": {"hour": [">=8", "<20"]}, "want": {"patrolled": true}},
              {"name": "rest", "when": {"hour": ">=20"}, "want": {"patrolled": false}}],
    "actions": [{"name": "walk_round", "effects": {"patrolled": true},
                 "do": {"follow_route": "round"}},
                {"name": "go_to_bed", "effects": {"patrolled": false}, "do": {"go_to": "bed"}}]})";

// The same watchman in a 3D town: his round in meters on the ground.
constexpr std::string_view kWatchman3D = R"({"format": 1, "extends": "watchman.npc.json",
    "routes": {"round": {"points": [[4, 0, 0], [4, 0, 4]], "closed": true}}})";

// A lost lamb: found and returned for a reward, or kept by a thief; it fails if the wolf eats it.
constexpr std::string_view kLamb = R"({"format": 1, "title": "The lost lamb",
    "start_when": {"met_mira": true},
    "stages": [
        {"id": "find", "journal": "Mira's lamb ran off into the hills.",
         "objectives": [{"id": "search", "text": "Find the lamb", "when": {"lamb_found": true}},
                        {"id": "wolf", "text": "Spot the wolf", "when": {"wolf_seen": true},
                         "optional": true}],
         "fail_when": {"lamb_eaten": true},
         "next": [{"when": {"kept_lamb": true}, "stage": "thief"}, {"stage": "return"}]},
        {"id": "return", "journal": "I found the lamb.",
         "objectives": [{"id": "give", "text": "Bring it to Mira",
                         "when": {"lamb_returned": true}}],
         "rewards": {"coins": "+5", "reputation": "+1"}},
        {"id": "thanks", "journal": "Mira was overjoyed.", "end": "success"},
        {"id": "thief", "journal": "I kept the lamb.", "end": "failure"}]})";

constexpr std::string_view kMira = R"({"format": 1,
    "start": [{"when": {"lamb_found": true}, "line": "found"}, {"line": "hello"}],
    "lines": {
        "hello": {"speaker": "Mira", "text": "Have you seen my lamb?",
                  "choices": [{"text": "I'll look for it.", "effects": {"met_mira": true},
                               "start_quest": "lamb.quest.json", "next": "thanks"},
                              {"text": "Here it is.", "when": {"lamb_found": true},
                               "next": "found"},
                              {"text": "No.", "next": "bye"}]},
        "thanks": {"speaker": "Mira", "text": "Thank you!", "effects": {"self.hopeful": true}},
        "found": {"speaker": "Mira", "text": "My lamb!", "effects": {"lamb_returned": true}},
        "bye": {"speaker": "Mira", "text": "Oh."}}})";

constexpr std::string_view kMiraBrain = R"({"format": 1, "facts": {"hopeful": false},
    "goals": [], "actions": []})";

} // namespace

TEST_CASE("a brain extends a shared one: traits, goals and actions by name") {
    MemoryFiles files;
    files.files["villager.npc.json"] = std::string(kVillager);
    files.files["guard.npc.json"] = std::string(kGuard);
    auto brain = npc::parse_brain(parse(kGuard), files.reader());
    REQUIRE(brain.ok());
    CHECK(brain.value().facts == Names{"alert", "brave", "fed", "tired"});
    CHECK(brain.value().defaults == std::vector<int>{0, 3, 0, 0});
    CHECK(brain.value().traits == std::vector<std::size_t>{1});
    REQUIRE(brain.value().goals.size() == 3);
    CHECK(brain.value().goals[0].name == "rest");
    CHECK(brain.value().goals[0].priority == 5.0F);
    CHECK(brain.value().goals[0].want.size() == 1); // kept from the shared brain
    CHECK(brain.value().priorities == std::vector<std::size_t>{0, 2, 1});
    Names actions;
    for (const npc::Action& action : brain.value().actions) {
        actions.push_back(action.name);
    }
    CHECK(actions == Names{"sleep", "eat", "look"});
    // A guard watches before eating; a villager just eats.
    CHECK(npc::choose(brain.value(), brain.value().defaults).goal->name == "watch");

    // Without the files it reads, extends can't be checked.
    const auto unread = npc::validate_brain(parse(kGuard));
    REQUIRE(unread.size() == 1);
    CHECK(unread[0].path == "/extends");

    // No action changes a trait, and a brain can't extend itself.
    const auto trait = npc::validate_brain(parse(R"({"format": 1, "extends": "villager.npc.json",
        "traits": {"brave": 1}, "actions": [{"name": "fear", "effects": {"brave": 0}}]})"),
                                           files.reader());
    REQUIRE(trait.size() == 1);
    CHECK(trait[0].path == "/actions/3/effects/brave");
    CHECK(trait[0].message ==
          "\"brave\" is a trait, which no action changes (in the brain with what it extends)");
    files.files["a.npc.json"] = R"({"format": 1, "extends": "b.npc.json"})";
    files.files["b.npc.json"] = R"({"format": 1, "extends": "a.npc.json"})";
    const auto loop = npc::validate_brain(parse(files.files["a.npc.json"]), files.reader());
    REQUIRE(loop.size() == 1);
    CHECK(loop[0].message == "\"b.npc.json\" extends itself");

    // A follow_route names a route the brain has; "remove" is only for brains that extend.
    const auto lost = npc::validate_brain(parse(R"({"format": 1, "facts": {},
        "goals": [{"name": "g", "want": {}, "remove": true}],
        "actions": [{"name": "a", "do": {"follow_route": "loop"}}]})"));
    REQUIRE(lost.size() == 2);
    CHECK(lost[0].path == "/goals/0/remove");
    CHECK(lost[1].path == "/actions/0/do/follow_route");
}

TEST_CASE("NPCs keep daily schedules on the npc clock and walk their routes, in 2D and 3D") {
    Scene scene({{"watchman.npc.json", std::string(kWatchman)},
                 {"watchman_3d.npc.json", std::string(kWatchman3D)}},
                {.day_seconds = 24.0, .start_hour = 6.0}); // an hour a second
    const entt::entity guard = scene.create("guard", R"({"Transform2D": {},
        "NpcAgent": {"brain": "watchman.npc.json", "speed": 20}})");
    const entt::entity guard3d = scene.create("guard3d", R"({"Transform3D": {},
        "NpcAgent": {"brain": "watchman_3d.npc.json", "speed": 8, "arrive_distance": 0.1}})");
    scene.create("bed", R"({"Transform2D": {"position": [0, -5]}})");

    // Before 8 o'clock neither goal applies.
    scene.run(1.5);
    CHECK(npc::clock(scene.world).hour == 7);
    CHECK(scene.world.ecs().get<world::NpcAgent>(guard).goal.empty());
    CHECK(near(place_of(scene, guard), {0.0F, 0.0F, 0.0F}));

    // At 8 the round starts: out to its first point, then the second, then home.
    scene.run(0.5 + 0.5);
    CHECK(scene.world.ecs().get<world::NpcAgent>(guard).goal == "patrol");
    CHECK(near(place_of(scene, guard), {10.0F, 0.0F, 0.0F}, 1.0));
    CHECK(near(place_of(scene, guard3d), {4.0F, 0.0F, 0.0F}, 0.2));
    scene.run(0.5);
    CHECK(near(place_of(scene, guard), {10.0F, 10.0F, 0.0F}, 1.0));
    CHECK(near(place_of(scene, guard3d), {4.0F, 0.0F, 4.0F}, 0.2));
    // A closed round ends where it began.
    scene.run(1.0);
    CHECK(near(place_of(scene, guard), {10.0F, 0.0F, 0.0F}, 1.0));
    CHECK(near(place_of(scene, guard3d), {4.0F, 0.0F, 0.0F}, 0.2));
    const auto& agent = scene.world.ecs().get<world::NpcAgent>(guard);
    CHECK(agent.facts.at("patrolled") == 1);
    CHECK(agent.waypoint == 0);
    CHECK_FALSE(agent.facts.contains("hour")); // the clock's, not the agent's

    // At 20 o'clock he goes to bed, and the next morning walks his round again.
    scene.run(10.5);
    CHECK(npc::clock(scene.world).hour == 20);
    scene.run(1.0);
    CHECK(near(place_of(scene, guard), {0.0F, -5.0F, 0.0F}));
    CHECK_FALSE(scene.world.ecs().get<world::NpcAgent>(guard).facts.contains("patrolled"));
    scene.run(11.0);
    CHECK(npc::clock(scene.world).day == 1);
    CHECK(npc::clock(scene.world).hour == 8);
    CHECK(scene.world.ecs().get<world::NpcAgent>(guard).goal == "patrol");
}

TEST_CASE("quests move through their stages as the story's facts change") {
    Scene scene({{"lamb.quest.json", std::string(kLamb)}});
    const entt::entity player =
        scene.create("player", R"({"QuestLog": {"quests": {"lamb.quest.json": {}}}})");
    // It waits for its start_when.
    scene.run(kTick);
    CHECK(scene.log(player).quests.at("lamb.quest.json").state == "waiting");
    npc::set_story_fact(scene.world, player, "met_mira", 1);
    CHECK(scene.log(player).quests.at("lamb.quest.json").stage == "find");
    npc::set_story_fact(scene.world, player, "wolf_seen", 1);
    CHECK(scene.log(player).quests.at("lamb.quest.json").done == Names{"wolf"});
    // The optional objective alone doesn't move it on; the lamb found does, and no thief kept it.
    npc::set_story_fact(scene.world, player, "lamb_found", 1);
    CHECK(scene.log(player).quests.at("lamb.quest.json").stage == "return");
    npc::set_story_fact(scene.world, player, "lamb_returned", 1);
    const world::QuestProgress& done = scene.log(player).quests.at("lamb.quest.json");
    CHECK(done.state == "succeeded");
    CHECK(done.reached == Names{"find", "return", "thanks"});
    CHECK(npc::story_fact(scene.world, player, "coins") == 5);
    CHECK(npc::story_fact(scene.world, player, "reputation") == 1);
    CHECK(scene.story() == Names{"quest_started", "quest_stage_changed find",
                                 "quest_objective_done find wolf",
                                 "quest_objective_done find search", "quest_stage_changed return",
                                 "quest_objective_done return give", "quest_stage_changed thanks",
                                 "quest_finished succeeded"});
    auto views = npc::quests_of(scene.world, player);
    REQUIRE(views.ok());
    REQUIRE(views.value().size() == 1);
    CHECK(views.value()[0].title == "The lost lamb");
    CHECK(views.value()[0].journal ==
          Names{"Mira's lamb ran off into the hills.", "I found the lamb.", "Mira was overjoyed."});

    // Another player keeps the lamb, and a third loses it to the wolf.
    const entt::entity thief = scene.create("thief", "{}");
    REQUIRE(npc::start_quest(scene.world, thief, "lamb.quest.json").ok());
    npc::set_story_fact(scene.world, thief, "kept_lamb", 1);
    npc::set_story_fact(scene.world, thief, "lamb_found", 1);
    CHECK(scene.log(thief).quests.at("lamb.quest.json").state == "failed");
    CHECK(scene.log(thief).quests.at("lamb.quest.json").stage == "thief");
    CHECK_FALSE(scene.log(thief).facts.contains("coins"));
    const entt::entity unlucky = scene.create("unlucky", "{}");
    REQUIRE(npc::start_quest(scene.world, unlucky, "lamb.quest.json").ok());
    npc::set_story_fact(scene.world, unlucky, "lamb_eaten", 1);
    CHECK(scene.log(unlucky).quests.at("lamb.quest.json").state == "failed");
    CHECK(scene.log(unlucky).quests.at("lamb.quest.json").stage == "find");
    CHECK_FALSE(npc::start_quest(scene.world, unlucky, "missing.quest.json").ok());
}

TEST_CASE("conversations go by choices that read and change the story and the NPC") {
    Scene scene({{"lamb.quest.json", std::string(kLamb)},
                 {"mira.dialogue.json", std::string(kMira)},
                 {"mira.npc.json", std::string(kMiraBrain)}});
    const entt::entity player = scene.create("player", "{}");
    const entt::entity mira = scene.create("mira", R"({"NpcAgent": {"brain": "mira.npc.json"}})");
    REQUIRE(npc::start_dialogue(scene.world, player, mira, "mira.dialogue.json").ok());
    auto line = npc::current_line(scene.world, player);
    REQUIRE(line.ok());
    CHECK(line.value().talking);
    CHECK(line.value().line == "hello");
    CHECK(line.value().speaker == "Mira");
    CHECK(line.value().entity == "mira");
    CHECK(line.value().choices == Names{"I'll look for it.", "No."}); // the lamb isn't found
    CHECK_FALSE(npc::advance_dialogue(scene.world, player).ok());     // a choice is needed
    CHECK_FALSE(npc::advance_dialogue(scene.world, player, 2).ok());  // two are shown

    REQUIRE(npc::advance_dialogue(scene.world, player, 0).ok());
    CHECK(npc::current_line(scene.world, player).value().line == "thanks");
    CHECK(npc::story_fact(scene.world, player, "met_mira") == 1);
    CHECK(scene.log(player).quests.at("lamb.quest.json").stage == "find");
    CHECK(npc::facts_of(scene.world, mira) == std::vector<int>{1});  // hopeful
    CHECK_FALSE(npc::advance_dialogue(scene.world, player, 0).ok()); // no choices here
    REQUIRE(npc::advance_dialogue(scene.world, player).ok());
    CHECK_FALSE(npc::current_line(scene.world, player).value().talking);
    CHECK(scene.world.ecs().get<world::Conversation>(player).dialogue.empty());

    // With the lamb found, the conversation starts at its other branch and ends the quest.
    npc::set_story_fact(scene.world, player, "lamb_found", 1);
    REQUIRE(npc::start_dialogue(scene.world, player, mira, "mira.dialogue.json").ok());
    CHECK(npc::current_line(scene.world, player).value().line == "found");
    CHECK(scene.log(player).quests.at("lamb.quest.json").state == "succeeded");
    REQUIRE(npc::advance_dialogue(scene.world, player).ok());
    const Names story = scene.story();
    CHECK(story.front() == "dialogue_line hello");
    CHECK(std::ranges::count(story, "dialogue_ended thanks") == 1);
    CHECK(std::ranges::count(story, "dialogue_ended found") == 1);
    CHECK_FALSE(npc::start_dialogue(scene.world, player, mira, "mira.dialogue.json", "nope").ok());
}

TEST_CASE("NPCs read and change the story's facts") {
    Scene scene({{"sword.quest.json", R"({"format": 1, "title": "A new sword",
        "stages": [
            {"id": "order", "objectives": [{"id": "ask", "text": "Order a sword",
                                            "when": {"sword_ordered": true}}]},
            {"id": "wait", "objectives": [{"id": "collect", "text": "Collect it",
                                           "when": {"sword_ready": true}}],
             "rewards": {"swords": "+1"}},
            {"id": "done", "end": "success"}]})"},
                 {"smith.npc.json", R"({"format": 1,
        "facts": {"story.sword_ordered": false, "story.sword_ready": false},
        "goals": [{"name": "forge", "when": {"story.sword_ordered": true},
                   "want": {"story.sword_ready": true}}],
        "actions": [{"name": "forge", "effects": {"story.sword_ready": true},
                     "do": {"wait": 1}}]})"}});
    const entt::entity player = scene.create("player", R"({"QuestLog": {}})");
    const entt::entity smith =
        scene.create("smith", R"({"NpcAgent": {"brain": "smith.npc.json"}})");
    REQUIRE(npc::start_quest(scene.world, player, "sword.quest.json").ok());
    CHECK(npc::story_log(scene.world, smith) == player);
    scene.run(0.5);
    CHECK(scene.world.ecs().get<world::NpcAgent>(smith).goal.empty());
    CHECK(npc::set_fact(scene.world, smith, "story.sword_ordered", 1)); // through the log
    CHECK(scene.log(player).quests.at("sword.quest.json").stage == "wait");
    scene.run(1.5);
    CHECK(scene.log(player).quests.at("sword.quest.json").state == "succeeded");
    CHECK(npc::story_fact(scene.world, player, "swords") == 1);
    CHECK(scene.world.ecs().get<world::NpcAgent>(smith).facts.empty()); // kept in the log
}

TEST_CASE("check_story finds quests that can't finish and lines nothing reaches") {
    npc::StoryFiles files;
    files.quests["lamb.quest.json"] = parse(kLamb);
    files.dialogues["mira.dialogue.json"] = parse(kMira);
    // Nothing sets lamb_found, so the lamb is never found and the quest stops at its first stage.
    const auto stuck = npc::check_story(files);
    const auto has = [&](const std::vector<npc::StoryProblem>& problems, std::string_view file,
                         std::string_view path, std::string_view start) {
        return std::ranges::any_of(problems, [&](const npc::StoryProblem& problem) {
            return problem.file == file && problem.path == path &&
                   problem.message.starts_with(start);
        });
    };
    CHECK(has(stuck, "lamb.quest.json", "/stages/0/objectives/0/when/lamb_found",
              "objective \"search\" never holds: nothing sets \"lamb_found\""));
    CHECK(has(stuck, "lamb.quest.json", "/stages/1", "no way through the quest reaches"));
    CHECK(has(stuck, "lamb.quest.json", "", "the quest can't be finished"));
    CHECK(has(stuck, "mira.dialogue.json", "/lines/found",
              "no way through the conversation reaches line \"found\""));
    CHECK(has(stuck, "mira.dialogue.json", "/lines/hello/choices/1/when/lamb_found",
              "this choice's when never holds"));

    // Game code finds lambs, spots wolves and keeps or loses them: everything can happen.
    json::Value lamb = parse(kLamb);
    lamb["external"] = json::Array{"lamb_found", "wolf_seen", "kept_lamb", "lamb_eaten"};
    files.quests["lamb.quest.json"] = std::move(lamb);
    for (const npc::StoryProblem& problem : npc::check_story(files)) {
        FAIL_CHECK(problem.file << problem.path << ": " << problem.message);
    }

    // A line nothing leads to, and a quest that isn't there.
    json::Value mira = parse(kMira);
    mira["lines"]["lost"] = parse(R"({"text": "...", "start_quest": "missing.quest.json"})");
    files.dialogues["mira.dialogue.json"] = std::move(mira);
    const auto found = npc::check_story(files);
    CHECK(found.size() == 2);
    CHECK(has(found, "mira.dialogue.json", "/lines/lost", "no way through"));
    CHECK(has(found, "mira.dialogue.json", "/lines/lost/start_quest",
              "no quest file \"missing.quest.json\""));

    // Files are checked against their schemas first.
    const auto bad = npc::validate_quest(parse(R"({"format": 1, "title": "t",
        "stages": [{"id": "a", "next": "b"}, {"id": "a", "end": "success",
                    "objectives": [], "rewards": {"x": "*2"}}]})"));
    REQUIRE(bad.size() == 5);
    CHECK(bad[0].path == "/stages/1/id");
    CHECK(bad[1].path == "/stages/0/next");
    CHECK(bad[2].path == "/stages/1/objectives");
    CHECK(bad[3].path == "/stages/1/rewards");
    CHECK(bad[4].path == "/stages/1/rewards/x");
    const auto lines = npc::validate_dialogue(parse(R"({"format": 1, "start": "a",
        "lines": {"a": {"text": "hi", "next": "b", "choices": [{"text": "x", "next": "zz"}]}}})"));
    REQUIRE(lines.size() == 3);
    CHECK(lines[0].path == "/lines/a/next");
    CHECK(lines[1].path == "/lines/a/next");
    CHECK(lines[2].path == "/lines/a/choices/0/next");
}

TEST_CASE("quest and dialogue commands drive a story through a session") {
    const std::string root = "npc_stories_project";
    std::filesystem::remove_all(root);
    REQUIRE(slash::platform::create_directories(root).ok());
    REQUIRE(slash::platform::write_file(root + "/project.json",
                                        R"({"format": 1, "name": "Lamb",
        "main_scene": "main.scene.json", "version": "1.0.0", "npc": {"start_hour": 9}})")
                .ok());
    REQUIRE(slash::platform::write_file(root + "/lamb.quest.json", std::string(kLamb)).ok());
    REQUIRE(slash::platform::write_file(root + "/mira.dialogue.json", std::string(kMira)).ok());
    REQUIRE(slash::platform::write_file(root + "/mira.npc.json", std::string(kMiraBrain)).ok());
    {
        world::ComponentRegistry components;
        world::register_builtin_components(components);
        command::Registry commands;
        command::register_builtin_commands(commands);
        command::SessionOptions options;
        options.project_root = root;
        options.capture_logs = false;
        command::Session session(components, commands, std::move(options));
        const auto ok = [&](std::string_view name, std::string_view params) {
            command::Reply reply = session.call(name, parse(params));
            REQUIRE_MESSAGE(reply.ok, reply.message);
            return json::write(reply.value, json::Style::Compact);
        };
        ok("batch", R"({"commands": [
            {"command": "entity.create", "params": {"id": "player"}},
            {"command": "entity.create", "params": {"id": "mira",
             "components": {"NpcAgent": {"brain": "mira.npc.json"}}}}]})");
        CHECK(npc::clock(session.world()).hour == 9);
        CHECK(
            ok("dialogue.start", R"({"id": "player", "speaker": "mira",
                                       "dialogue": "mira.dialogue.json"})") ==
            R"({"choices":["I'll look for it.","No."],"dialogue":"mira.dialogue.json","line":"hello","speaker":"Mira","talking":true,"text":"Have you seen my lamb?"})");
        CHECK(
            ok("dialogue.choose", R"({"id": "player", "choice": 0})") ==
            R"({"dialogue":"mira.dialogue.json","line":"thanks","speaker":"Mira","talking":true,"text":"Thank you!"})");
        CHECK(ok("dialogue.choose", R"({"id": "player"})") == R"({"talking":false})");
        CHECK(
            ok("quest.set", R"({"id": "player", "facts": {"lamb_found": true}})") ==
            R"({"facts":{"lamb_found":1,"met_mira":1},"quests":[{"journal":["Mira's lamb ran off into the hills.","I found the lamb."],"objectives":[{"done":false,"id":"give","text":"Bring it to Mira"}],"quest":"lamb.quest.json","stage":"return","state":"active","title":"The lost lamb"}]})");
        CHECK(ok("quest.state", R"({"id": "player"})")
                  .starts_with(R"({"facts":{"lamb_found":1,"met_mira":1},"quests":[{"journal")"));
        // Undo takes the facts back.
        ok("undo", "{}");
        CHECK(npc::story_fact(session.world(), session.world().find("player"), "lamb_found") == 0);
        // Nothing sets lamb_found but this command, so the project's check says so.
        const std::string checked = ok("quest.check", "{}");
        CHECK(checked.find("objective \\\"search\\\" never holds") != std::string::npos);
        command::Reply bad = session.call("quest.start", parse(R"({"id": "player",
                                                                   "quest": "lamb.json"})"));
        CHECK_FALSE(bad.ok);
        CHECK(bad.message == "/quest: quests are .quest.json files");
    }
    std::filesystem::remove_all(root);
}
