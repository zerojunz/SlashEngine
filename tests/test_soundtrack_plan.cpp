#include "slash/command/builtin.hpp"
#include "slash/command/session.hpp"
#include "slash/core/json.hpp"
#include "slash/platform/file.hpp"
#include "slash/soundtrack/commands.hpp"
#include "slash/soundtrack/plan.hpp"
#include "slash/world/component_registry.hpp"
#include "slash/world/components.hpp"

#include <doctest/doctest.h>

#include <filesystem>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace json = slash::json;
namespace soundtrack = slash::soundtrack;

namespace {

json::Value parse(std::string_view text) {
    auto parsed = json::parse(text);
    REQUIRE_MESSAGE(parsed.ok(), (parsed.ok() ? std::string() : parsed.error().message));
    return std::move(parsed).value();
}

std::string compact(const json::Value& value) {
    return json::write(value, json::Style::Compact);
}

} // namespace

TEST_CASE("plan drafts a cue for each scene, beat and state with no music") {
    const json::Object entries = parse(R"({
        "place/lowtown": {"name": "Lowtown"},
        "place/docks": {"name": "The Docks", "region": "place/lowtown"},
        "place/tower": {"name": "The Tower"},
        "faction/couriers": {"name": "The Couriers"},
        "character/mira": {"name": "Mira", "faction": "faction/couriers", "home": "place/docks"},
        "character/vex": {"name": "Vex"},
        "beat/arrival": {"name": "Arrival", "place": "place/docks",
                         "characters": ["character/mira"]},
        "beat/duel": {"name": "The duel", "place": "place/tower", "characters": ["character/vex"]},
        "beat/finale": {"name": "Finale", "links": ["scenes/roof.scene.json"]},
        "theme/lowtown": {"name": "Lowtown", "subjects": ["place/lowtown"]},
        "theme/mira": {"name": "Mira's theme", "subjects": ["character/mira"]},
        "theme/menu": {"name": "Menu", "links": ["scenes/menu.scene.json"]},
        "cue/docks": {"name": "Old docks", "scenes": ["scenes/old.scene.json"]},
        "cue/title": {"name": "Title", "scenes": ["scenes/title.scene.json"], "states": ["paused"]}
    })")
                                     .as_object();
    const std::vector<soundtrack::PlanScene> scenes = {
        {.path = "scenes/Boss Room.scene.json", .story_ids = {}},
        {.path = "scenes/docks.scene.json", .story_ids = {"beat/arrival"}},
        {.path = "scenes/menu.scene.json", .story_ids = {}},
        // Mira lives in the docks, so the place's theme comes before hers.
        {.path = "scenes/roof.scene.json", .story_ids = {"character/mira"}},
        {.path = "scenes/title.scene.json", .story_ids = {"character/vex"}},
    };
    const soundtrack::Plan plan = soundtrack::plan_cues(entries, scenes, {"combat", "paused"});

    std::vector<std::string> edits;
    for (const json::Value& edit : plan.edits) {
        edits.push_back(compact(edit));
    }
    CHECK(
        edits ==
        std::vector<std::string>{
            R"({"id":"cue/boss_room","set":{"name":"Boss room","scenes":["scenes/Boss Room.scene.json"],)"
            R"("summary":"Plays in scenes/Boss Room.scene.json."},"version":null})",
            R"({"id":"cue/docks_2","set":{"beats":["beat/arrival"],"name":"Docks",)"
            R"("scenes":["scenes/docks.scene.json"],"summary":"Plays in scenes/docks.scene.json.",)"
            R"("theme":"theme/lowtown"},"version":null})",
            R"({"id":"cue/menu","set":{"name":"Menu","scenes":["scenes/menu.scene.json"],)"
            R"("summary":"Plays in scenes/menu.scene.json.","theme":"theme/menu"},"version":null})",
            R"({"id":"cue/roof","set":{"beats":["beat/finale"],"name":"Roof",)"
            R"("scenes":["scenes/roof.scene.json"],"summary":"Plays in scenes/roof.scene.json.",)"
            R"("theme":"theme/lowtown"},"version":null})",
            R"({"id":"cue/duel","set":{"beats":["beat/duel"],"name":"The duel",)"
            R"("summary":"Plays during beat/duel."},"version":null})",
            R"({"id":"cue/combat","set":{"name":"Combat","states":["combat"],)"
            R"("summary":"Plays in the combat state."},"version":null})",
        });
    // Nothing stands for the tower, where the duel happens.
    CHECK(plan.unthemed == std::vector<std::string>{"place/tower"});

    // A character's own theme plays when nothing about where the scene is set has one.
    json::Object homeless = entries;
    homeless["character/mira"] = parse(R"({"name": "Mira"})");
    const soundtrack::Plan hers = soundtrack::plan_cues(
        homeless, {{.path = "scenes/roof.scene.json", .story_ids = {"character/mira"}}}, {});
    REQUIRE(!hers.edits.empty());
    CHECK(hers.edits[0].find("set")->find("theme")->as_string() == "theme/mira");
}

TEST_CASE("soundtrack.plan drafts edits storyboard.edit applies, after which nothing is missing") {
    const std::string root = "command_soundtrack_plan_project";
    std::filesystem::remove_all(root);
    REQUIRE(slash::platform::create_directories(root + "/scenes").ok());
    REQUIRE(slash::platform::create_directories(root + "/storyboard").ok());
    REQUIRE(slash::platform::create_directories(root + "/build/export/scenes").ok());
    const auto write = [&](const std::string& path, std::string_view text) {
        REQUIRE(slash::platform::write_file(root + "/" + path, text).ok());
    };
    write("storyboard/world.json", R"({"format": 1, "entries": {
        "place/harbor": {"name": "The Harbor"},
        "character/ada": {"name": "Ada", "home": "place/harbor"}}})");
    write("storyboard/story.json", R"({"format": 1, "entries": {
        "beat/storm": {"name": "The storm", "place": "place/harbor"}}})");
    write("storyboard/music.json", R"({"format": 1, "entries": {
        "theme/sea": {"name": "The sea", "subjects": ["place/harbor"], "key": "E minor",
                      "tempo": 84},
        "cue/menu": {"name": "Menu", "scenes": ["scenes/menu.scene.json"]}}})");
    write("scenes/menu.scene.json", R"({"entities": {}, "format": 1})");
    write("scenes/harbor.scene.json", R"({"format": 1, "entities": {"ada": {"components": {
        "StoryRef": {"ids": ["character/ada"]}}}}})");
    write("build/export/scenes/harbor.scene.json", R"({"entities": {}, "format": 1})");

    slash::world::ComponentRegistry components;
    slash::world::register_builtin_components(components);
    slash::command::Registry commands;
    slash::command::register_builtin_commands(commands);
    soundtrack::register_commands(commands);
    slash::command::Session session(
        components, commands,
        slash::command::SessionOptions{.project_root = root, .capture_logs = false});
    const auto call = [&](std::string_view name, std::string_view params) {
        return session.call(name, parse(params));
    };

    slash::command::Reply reply = call("storyboard.check", "{}");
    REQUIRE_MESSAGE(reply.ok, reply.message);
    CHECK(compact(reply.value["warnings"]) ==
          R"([{"file":"scenes/harbor.scene.json","message":"no cue plays in this scene; )"
          R"(soundtrack.plan drafts one","path":""},{"file":"storyboard/story.json",)"
          R"("message":"no cue plays during beat/storm; soundtrack.plan drafts one",)"
          R"("path":"/entries/beat~1storm"}])");

    reply = call("soundtrack.plan", R"({"states": ["combat"]})");
    REQUIRE_MESSAGE(reply.ok, reply.message);
    CHECK(compact(reply.value) ==
          R"({"edits":[{"id":"cue/harbor","set":{"name":"Harbor",)"
          R"("scenes":["scenes/harbor.scene.json"],"summary":"Plays in scenes/harbor.scene.json.",)"
          R"("theme":"theme/sea"},"version":null},)"
          R"({"id":"cue/storm","set":{"beats":["beat/storm"],"name":"The storm",)"
          R"("summary":"Plays during beat/storm.","theme":"theme/sea"},"version":null},)"
          R"({"id":"cue/combat","set":{"name":"Combat","states":["combat"],)"
          R"("summary":"Plays in the combat state."},"version":null}]})");

    json::Value edit;
    edit["edits"] = reply.value["edits"];
    edit["why"] = "Plan the music";
    reply = session.call("storyboard.edit", edit);
    REQUIRE_MESSAGE(reply.ok, reply.message);
    reply = call("storyboard.check", "{}");
    REQUIRE_MESSAGE(reply.ok, reply.message);
    CHECK(reply.value.find("warnings") == nullptr);
    reply = call("soundtrack.plan", R"({"states": ["combat"]})");
    REQUIRE_MESSAGE(reply.ok, reply.message);
    CHECK(reply.value["edits"].as_array().empty());

    // The open scene counts as it is now, before it is saved.
    reply = call("scene.new", R"({"path": "scenes/cellar.scene.json"})");
    REQUIRE_MESSAGE(reply.ok, reply.message);
    reply = call("soundtrack.plan", "{}");
    REQUIRE_MESSAGE(reply.ok, reply.message);
    REQUIRE(reply.value["edits"].as_array().size() == 1);
    CHECK(reply.value["edits"].as_array()[0]["id"].as_string() == "cue/cellar");

    // A storyboard that doesn't load plans nothing.
    write("storyboard/music.json", R"({"format": 1, "entries": {"cue/x": {"plays": "twice"}}})");
    reply = call("soundtrack.plan", "{}");
    REQUIRE_FALSE(reply.ok);
    CHECK(reply.message.find("storyboard/music.json at /entries/cue~1x") == 0);
    CHECK(reply.message.find("storyboard.check lists every problem") != std::string::npos);
}
