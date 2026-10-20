#include "slash/command/builtin.hpp"
#include "slash/command/command.hpp"
#include "slash/command/session.hpp"
#include "slash/core/json.hpp"
#include "slash/gameplay/host.hpp"
#include "slash/input/input.hpp"
#include "slash/platform/file.hpp"
#include "slash/world/component_registry.hpp"
#include "slash/world/components.hpp"
#include "slash/world/game_module.hpp"
#include "slash/world/scene.hpp"
#include "slash/world/world.hpp"

#include <doctest/doctest.h>

#include <cmath>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace json = slash::json;
namespace command = slash::command;
namespace world = slash::world;
namespace fs = std::filesystem;

namespace playtest_game {

struct Runner {
    float speed = 128.0F;
    int coins = 0;
};
SLASH_COMPONENT(Runner, speed, coins)

struct Coin {
    int value = 1;
};
SLASH_COMPONENT(Coin, value)

constexpr std::string_view kCoinPrefab = "prefabs/coin.prefab.json";

// Runs right while move_right is held, emits "jumped" when jump is pressed, and picks up coins
// it passes; a coin from the prefab is replaced by another 48 pixels on.
void run(world::World& w, double seconds) {
    std::vector<std::pair<std::string, entt::entity>> collected;
    for (const entt::entity runner : w.ecs().view<world::Transform2D, Runner>()) {
        auto& transform = w.ecs().get<world::Transform2D>(runner);
        auto& state = w.ecs().get<Runner>(runner);
        if (slash::input::is_action_pressed(w, "move_right")) {
            transform.position.x += state.speed * static_cast<float>(seconds);
        }
        if (slash::input::is_action_just_pressed(w, "jump")) {
            w.emit("jumped", w.id_of(runner));
        }
        for (const entt::entity coin : w.ecs().view<world::Transform2D, Coin>()) {
            const auto& at = w.ecs().get<world::Transform2D>(coin).position;
            if (std::abs(at.x - transform.position.x) <= 2.0F) {
                state.coins += w.ecs().get<Coin>(coin).value;
                collected.emplace_back(w.id_of(coin), coin);
            }
        }
    }
    for (const auto& [id, coin] : collected) {
        const float x = w.ecs().get<world::Transform2D>(coin).position.x;
        const bool from_prefab = w.ecs().all_of<world::PrefabLink>(coin);
        w.emit("coin_collected", id);
        w.destroy(coin);
        if (from_prefab && !id.starts_with("next_")) {
            const entt::entity next = w.create("next_" + id).value();
            if (world::instantiate_prefab(w, next, kCoinPrefab)) {
                w.ecs().get<world::Transform2D>(next).position.x = x + 48.0F;
            }
        }
    }
}

void module(world::GameModule& game) {
    game.add_component<Runner>().set_doc("Runs right.");
    game.add_component<Coin>().set_doc("Picked up when passed.");
    game.add_system("run", run);
}

} // namespace playtest_game

namespace {

json::Value parse(std::string_view text) {
    auto parsed = json::parse(text);
    REQUIRE_MESSAGE(parsed.ok(), (parsed.ok() ? std::string() : parsed.error().message));
    return std::move(parsed).value();
}

void write(const fs::path& path, std::string_view text) {
    fs::create_directories(path.parent_path());
    REQUIRE(slash::platform::write_file(path.string(), text).ok());
}

// A project with the two actions the bot presses and the coin prefab.
fs::path write_project(std::string_view name) {
    const fs::path root = fs::absolute(name);
    fs::remove_all(root);
    write(root / "project.json", R"({"format": 1, "name": "Runner", "input": {
        "jump": {"events": [{"physical_key": "Space", "type": "key"}]},
        "move_right": {"events": [{"physical_key": "D", "type": "key"}]}}})");
    write(root / playtest_game::kCoinPrefab,
          R"({"components": {"Coin": {}, "Transform2D": {}}, "format": 1})");
    return root;
}

struct Game {
    world::ComponentRegistry components;
    command::Registry commands;
    std::unique_ptr<slash::gameplay::Host> host;
    std::unique_ptr<command::Session> session;

    explicit Game(const fs::path& project) {
        world::register_builtin_components(components);
        command::register_builtin_commands(commands);
        host = std::make_unique<slash::gameplay::Host>(components, playtest_game::module);
        session = std::make_unique<command::Session>(
            components, commands,
            command::SessionOptions{
                .project_root = project.string(), .capture_logs = false, .gameplay = host.get()});
    }

    json::Value call(std::string_view name, std::string_view params) const {
        command::Reply reply = session->call(name, parse(params));
        REQUIRE_MESSAGE(reply.ok, reply.message);
        return std::move(reply.value);
    }

    command::Reply fail(std::string_view params) const {
        command::Reply reply = session->call("playtest.run", parse(params));
        CHECK_FALSE(reply.ok);
        return reply;
    }
};

// A runner at x = 0 and coins where it passes, made by commands and never saved.
void build_level(const Game& game) {
    game.call("batch", R"({"commands": [
        {"command": "entity.create", "params": {"id": "runner",
            "components": {"Transform2D": {}, "Runner": {}}}},
        {"command": "entity.create", "params": {"id": "coin_a",
            "components": {"Transform2D": {"position": [48, 0]}, "Coin": {}}}},
        {"command": "entity.create", "params": {"id": "coin_b",
            "components": {"Transform2D": {"position": [148, 0]}, "Coin": {}}}}]})");
}

} // namespace

TEST_CASE("playtest.run sweeps values over runs of the open world and sums them up") {
    const Game game(write_project("playtest_sweep"));
    build_level(game);

    // Ticks of 1/64 s move the runner 2 or 4 pixels each, so it reaches x = 300 exactly.
    CHECK(game.call("playtest.run", R"({"vary": {"runner/Runner/speed": [128, 256]},
        "bot": [{"action": "move_right"}], "tick_seconds": 0.015625,
        "end": {"far": {"with": ["Runner"],
            "where": [{"component": "Transform2D", "field": "position.x", "op": ">=",
                       "value": 300}]}},
        "measure": ["runner/Transform2D/position/x", "*/Coin", "runner/Runner/coins",
                    "coin_a/Coin/value"]})") == parse(R"({"runs": 2, "results": [
        {"values": {"runner/Runner/speed": 128}, "ends": {"far": 1, "timeout": 0},
         "ticks": {"min": 150, "mean": 150, "max": 150}, "events": {"coin_collected": 2},
         "measure": {"runner/Transform2D/position/x": {"min": 300, "mean": 300, "max": 300},
                     "*/Coin": {"min": 0, "mean": 0, "max": 0},
                     "runner/Runner/coins": {"min": 2, "mean": 2, "max": 2},
                     "coin_a/Coin/value": {"missing": 1}}},
        {"values": {"runner/Runner/speed": 256}, "ends": {"far": 1, "timeout": 0},
         "ticks": {"min": 75, "mean": 75, "max": 75}, "events": {"coin_collected": 2},
         "measure": {"runner/Transform2D/position/x": {"min": 300, "mean": 300, "max": 300},
                     "*/Coin": {"min": 0, "mean": 0, "max": 0},
                     "runner/Runner/coins": {"min": 2, "mean": 2, "max": 2},
                     "coin_a/Coin/value": {"missing": 1}}}]})"));

    // The runs had their own worlds: the session's hasn't moved.
    CHECK(game.session->world().tick() == 0);
    CHECK(game.call("entity.get", R"({"id": "runner", "components": ["Transform2D"],
        "omit_defaults": true})") ==
          parse(R"({"components": {"Transform2D": {}}, "id": "runner"})"));

    // With no bot nothing moves, and runs with no end condition met are timeouts. "*" sets every
    // entity with the component, and count names the events to count.
    CHECK(game.call("playtest.run", R"({"vary": {"*/Coin/value": [3]}, "ticks": 10,
        "end": {"never": {"with": ["Runner"], "until": "none"}}, "count": ["jumped"],
        "measure": ["coin_a/Coin/value", "runner/Runner/coins"]})") ==
          parse(R"({"runs": 1, "results": [{"values": {"*/Coin/value": 3},
              "ends": {"never": 0, "timeout": 1}, "ticks": {"min": 10, "mean": 10, "max": 10},
              "events": {"jumped": 0},
              "measure": {"coin_a/Coin/value": {"min": 3, "mean": 3, "max": 3},
                          "runner/Runner/coins": {"min": 0, "mean": 0, "max": 0}}}]})"));
}

TEST_CASE("playtest.run's bot presses on a schedule, while entities match, and at random") {
    const Game game(write_project("playtest_bot"));
    build_level(game);

    // Every 30 ticks from 10 until 100, for 5 ticks each: 3 presses. While the runner is between
    // x = 100 and 140: held from when it gets there, so one press.
    CHECK(game.call("playtest.run", R"({"ticks": 120, "tick_seconds": 0.015625, "bot": [
        {"action": "jump", "every": 30, "from": 10, "to": 100, "for": 5}]})") ==
          parse(R"({"runs": 1, "results": [{"ticks": {"min": 120, "mean": 120, "max": 120},
              "events": {"jumped": 3}}]})"));
    CHECK(game.call("playtest.run", R"({"ticks": 120, "tick_seconds": 0.015625, "count": ["jumped"],
        "bot": [{"action": "move_right"}, {"action": "jump", "when": {"with": ["Runner"], "where": [
            {"component": "Transform2D", "field": "position.x", "op": ">=", "value": 100},
            {"component": "Transform2D", "field": "position.x", "op": "<", "value": 140}]}}]})") ==
          parse(R"({"runs": 1, "results": [{"ticks": {"min": 120, "mean": 120, "max": 120},
              "events": {"jumped": 1}}]})"));

    // Chance presses differ from run to run, the same on every call, and with another seed.
    const std::string random = R"({"runs": 8, "ticks": 200, "count": ["jumped"],
        "bot": [{"action": "jump", "chance": 0.05}]})";
    const json::Value first = game.call("playtest.run", random);
    CHECK(game.call("playtest.run", random) == first);
    const json::Value& jumped =
        *first.find("results")->as_array().at(0).find("events")->find("jumped");
    CHECK(jumped.as_double() > 2.0);
    CHECK(jumped.as_double() < 20.0);
    CHECK(game.call("playtest.run", R"({"runs": 8, "ticks": 200, "count": ["jumped"], "seed": 9,
        "bot": [{"action": "jump", "chance": 0.05}]})") != first);
}

TEST_CASE("playtest.run sets a prefab's value on its instances, made before and during the run") {
    const fs::path project = write_project("playtest_prefab");
    write(project / "level.scene.json", R"({"entities": {
        "coin": {"extends": "prefabs/coin.prefab.json",
                 "components": {"Transform2D": {"position": [48, 0]}}},
        "runner": {"components": {"Runner": {}, "Transform2D": {}}}}, "format": 1})");
    const Game game(project);
    game.call("scene.open", R"({"path": "level.scene.json"})");

    // The coin in the scene and the one that replaces it while the run plays both have the value.
    const std::string params = R"({"vary": {"prefabs/coin.prefab.json/Coin/value": [1, 5]},
        "bot": [{"action": "move_right"}], "ticks": 60, "tick_seconds": 0.015625,
        "count": ["coin_collected"], "measure": ["runner/Runner/coins"]})";
    const json::Value expected = parse(R"({"runs": 2, "results": [
        {"values": {"prefabs/coin.prefab.json/Coin/value": 1},
         "ticks": {"min": 60, "mean": 60, "max": 60}, "events": {"coin_collected": 2},
         "measure": {"runner/Runner/coins": {"min": 2, "mean": 2, "max": 2}}},
        {"values": {"prefabs/coin.prefab.json/Coin/value": 5},
         "ticks": {"min": 60, "mean": 60, "max": 60}, "events": {"coin_collected": 2},
         "measure": {"runner/Runner/coins": {"min": 10, "mean": 10, "max": 10}}}]})");
    CHECK(game.call("playtest.run", params) == expected);
    // The same from the scene file.
    json::Value from_file = parse(params);
    from_file["scene"] = "level.scene.json";
    command::Reply reply = game.session->call("playtest.run", from_file);
    REQUIRE_MESSAGE(reply.ok, reply.message);
    CHECK(reply.value == expected);
    // The prefab file is as it was.
    CHECK(slash::platform::read_file((project / playtest_game::kCoinPrefab).string()).value() ==
          R"({"components": {"Coin": {}, "Transform2D": {}}, "format": 1})");
}

TEST_CASE("playtest.run points at what is wrong in its params") {
    const Game game(write_project("playtest_errors"));
    build_level(game);
    const auto message = [&](std::string_view params) { return game.fail(params).message; };

    CHECK(message(R"({"bot": [{"action": "jmp"}]})") ==
          R"(/bot/0/action: no input action is named "jmp"; did you mean "jump"?)");
    CHECK(message(R"({"bot": [{"action": "jump", "from": 10, "to": 10}]})") ==
          "/bot/0/to: must be after from");
    CHECK(message(R"({"vary": {"runner/Runer/speed": [1]}})") ==
          "/vary/runner~1Runer~1speed: name an entity, then a component and its field, as "
          R"("player/Transform2D/position/y"; there is no component "Runer"; did you mean )"
          R"("Runner"?)");
    CHECK(message(R"({"vary": {"runner/Runner/sped": [1]}})") ==
          R"(/vary/runner~1Runner~1sped: Runner has no field "sped"; did you mean "speed"?)");
    CHECK(message(R"({"vary": {"runner/Runner/speed": [128, "fast"]}})")
              .starts_with("/vary/runner~1Runner~1speed/1: "));
    CHECK(message(R"({"vary": {"runnr/Runner/speed": [1]}})") ==
          R"(/vary/runnr~1Runner~1speed: no entity has the id "runnr"; did you mean "runner"?)");
    CHECK(message(R"({"vary": {"coin_a/Runner/speed": [1]}})") ==
          "/vary/coin_a~1Runner~1speed: coin_a has no Runner");
    CHECK(message(R"({"vary": {"missing.prefab.json/Coin/value": [1]}})")
              .starts_with("/vary/missing.prefab.json~1Coin~1value: "));
    CHECK(message(R"({"vary": {"runner/Runner/speed": [1, 2, 3, 4, 5]}, "runs": 64})") ==
          "/vary: 5 combinations of values times 64 runs of each is 320 runs; at most 256");
    CHECK(message(R"({"end": {"timeout": {"with": ["Runner"]}}})") ==
          "/end/timeout: \"timeout\" names the runs that meet no end condition; pick another "
          "name");
    CHECK(message(R"({"end": {"won": {}}})") ==
          "/end/won: give with, without or where: the entities to look for");
    CHECK(message(R"({"end": {"won": {"with": ["Runr"]}}})").starts_with("/end/won/with/0: "));
    CHECK(message(R"({"measure": ["*/Runner/speed"]})") ==
          "/measure/0: \"*\" counts the entities with a component, as \"*/Enemy\"; name an "
          "entity to read a field");
    CHECK(message(R"({"scene": "nowhere.scene.json"})").starts_with("/scene: "));
    CHECK(message(R"({"ticks": 0})").starts_with("/ticks: "));
}
