#include "slash/audio/audio.hpp"
#include "slash/command/builtin.hpp"
#include "slash/command/command.hpp"
#include "slash/command/session.hpp"
#include "slash/core/json.hpp"
#include "slash/core/json_schema.hpp"
#include "slash/core/log.hpp"
#include "slash/platform/file.hpp"
#include "slash/platform/platform.hpp"
#include "slash/platform/run.hpp"
#include "slash/render/device.hpp"
#include "slash/render/image.hpp"
#include "slash/world/component_registry.hpp"
#include "slash/world/components.hpp"

#include <doctest/doctest.h>

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace cmdtest {

struct Health {
    float current = 100.0F;
    float max = 100.0F;
};
SLASH_COMPONENT(Health, current, max)

struct Follow {
    entt::entity target = entt::null;
    std::string mode = "chase";
};
SLASH_COMPONENT(Follow, target, mode)

} // namespace cmdtest

namespace json = slash::json;
namespace world = slash::world;
namespace command = slash::command;
namespace render = slash::render;
using command::Capability;

namespace {

json::Value parse(std::string_view text) {
    auto parsed = json::parse(text);
    REQUIRE_MESSAGE(parsed.ok(), (parsed.ok() ? std::string() : parsed.error().message));
    return std::move(parsed).value();
}

struct Engine {
    world::ComponentRegistry components;
    command::Registry commands;
    std::unique_ptr<command::Session> session;

    explicit Engine(command::SessionOptions options = {}, bool capture_logs = false) {
        world::register_builtin_components(components);
        components.add<cmdtest::Health>().describe("current", {.doc = "Hit points.", .min = 0.0});
        components.add<cmdtest::Follow>();
        command::register_builtin_commands(commands);
        options.capture_logs = capture_logs;
        session = std::make_unique<command::Session>(components, commands, std::move(options));
    }

    command::Reply call(std::string_view name, std::string_view params = "{}") {
        return session->call(name, parse(params));
    }

    json::Value ok(std::string_view name, std::string_view params = "{}") {
        command::Reply reply = call(name, params);
        REQUIRE_MESSAGE(reply.ok, reply.message);
        return std::move(reply.value);
    }

    command::Reply fails(std::string_view name, std::string_view params, int expected_code) {
        command::Reply reply = call(name, params);
        REQUIRE_FALSE(reply.ok);
        CHECK_MESSAGE(reply.code == expected_code, reply.message);
        return reply;
    }

    json::Value scene() { return ok("scene.get").find("scene")->find("entities")->as_object(); }
};

bool contains(std::string_view text, std::string_view part) {
    return text.find(part) != std::string_view::npos;
}

} // namespace

TEST_CASE("the registry keeps commands sorted by name") {
    Engine engine;
    std::vector<std::string> names;
    for (const command::Command& c : engine.commands.commands()) {
        names.push_back(c.name);
    }
    CHECK(std::ranges::is_sorted(names));
    CHECK(engine.commands.find("entity.create") != nullptr);
    CHECK(engine.commands.find("entity.creat") == nullptr);
}

TEST_CASE("capabilities parse from a comma-separated list") {
    CHECK(command::parse_capabilities("all").value().size() == 5);
    CHECK(command::parse_capabilities("read,run").value() ==
          std::vector<Capability>{Capability::Read, Capability::Run});
    CHECK(command::parse_capabilities("read,build").value() ==
          std::vector<Capability>{Capability::Read, Capability::Build});
    CHECK_FALSE(command::parse_capabilities("read,write").ok());
}

TEST_CASE("engine.describe lists components with defaults and command names") {
    Engine engine;
    const json::Value result = engine.ok("engine.describe");
    CHECK(result.find("engine")->find("name")->as_string() == "SlashEngine");
    const json::Value& commands = *result.find("commands");
    CHECK(std::ranges::find(commands.as_array(), json::Value("world.query")) !=
          commands.as_array().end());
    bool found = false;
    for (const json::Value& component : result.find("components")->as_array()) {
        if (component.find("name")->as_string() == "Health") {
            found = true;
            CHECK(*component.find("defaults") == parse(R"({"current": 100, "max": 100})"));
        }
    }
    CHECK(found);

    const json::Value full = engine.ok("engine.describe", R"({"full": true})");
    CHECK(full.find("commands")->as_array().front().find("params") != nullptr);
}

TEST_CASE("unknown commands suggest the closest name") {
    Engine engine;
    const auto reply = engine.fails("world.qurey", "{}", command::code::kMethodNotFound);
    CHECK(contains(reply.message, "did you mean \"world.query\""));
}

TEST_CASE("params are checked against the command's schema") {
    Engine engine;
    auto reply = engine.fails("step", R"({"ticks": "ten"})", command::code::kInvalidParams);
    CHECK(reply.data.find("path")->as_string() == "/ticks");

    reply = engine.fails("step", R"({"tiks": 10})", command::code::kInvalidParams);
    CHECK(contains(reply.message, "did you mean \"ticks\""));

    reply =
        engine.fails("step", R"({"ticks": 0, "tick_seconds": -1})", command::code::kInvalidParams);
    CHECK(reply.data.find("errors")->size() == 2);

    // Null params count as an empty object.
    CHECK(engine.session->call("history.get", json::Value()).ok);
}

TEST_CASE("entity.create applies components and returns the id") {
    Engine engine;
    CHECK(engine.ok("entity.create", R"({"id": "player", "components": {
            "Transform2D": {"position": [64, 32]}, "Health": {"current": 50}}})") ==
          parse(R"({"id": "player"})"));
    CHECK(engine.ok("entity.create").find("id")->as_string() == "entity_1");
    CHECK(engine.scene() == parse(R"({
        "entity_1": {},
        "player": {"components": {"Health": {"current": 50}, "Transform2D": {"position": [64, 32]}}}
    })"));
}

TEST_CASE("component errors name the field and leave the world unchanged") {
    Engine engine;
    engine.ok("entity.create", R"({"id": "player"})");

    auto reply = engine.fails(
        "entity.create", R"({"id": "enemy", "components": {"Transform2D": {"positon": [1, 2]}}})",
        command::code::kInvalidParams);
    CHECK(reply.data.find("path")->as_string() == "/components/Transform2D/positon");
    CHECK(contains(reply.message, "did you mean \"position\""));

    reply = engine.fails("entity.create", R"({"id": "enemy", "components": {"Helth": {}}})",
                         command::code::kNotFound);
    CHECK(contains(reply.message, "did you mean \"Health\""));

    reply = engine.fails("entity.create",
                         R"({"id": "enemy", "components": {"Health": {"current": -5}}})",
                         command::code::kInvalidParams);
    CHECK(reply.data.find("path")->as_string() == "/components/Health/current");

    reply = engine.fails("entity.create",
                         R"({"id": "enemy", "components": {"Follow": {"target": "ghost"}}})",
                         command::code::kNotFound);
    CHECK(reply.data.find("path")->as_string() == "/components/Follow/target");

    engine.fails("entity.create", R"({"id": "player"})", command::code::kInvalidArgument);

    CHECK(engine.session->world().size() == 1);
    CHECK(engine.session->undo_labels() == std::vector<std::string>{"entity.create"});
}

TEST_CASE("entity.get and entity.destroy find entities by id") {
    Engine engine;
    engine.ok(
        "entity.create",
        R"({"id": "player", "components": {"Transform2D": {"rotation": 1.5}, "Health": {}}})");

    const json::Value all = engine.ok("entity.get", R"({"id": "player"})");
    CHECK(*all.find("components")->find("Transform2D") ==
          parse(R"({"position": [0, 0], "rotation": 1.5, "scale": [1, 1], "skew": 0,
                    "children": []})"));

    const json::Value compact =
        engine.ok("entity.get", R"({"id": "player", "components": ["Transform2D"],
                                    "omit_defaults": true})");
    CHECK(*compact.find("components") == parse(R"({"Transform2D": {"rotation": 1.5}})"));

    const auto reply = engine.fails("entity.get", R"({"id": "playr"})", command::code::kNotFound);
    CHECK(contains(reply.message, "did you mean \"player\""));

    engine.ok("entity.destroy", R"({"id": "player"})");
    CHECK(engine.session->world().size() == 0);
}

TEST_CASE("component.set reports only what changed") {
    Engine engine;
    engine.ok("entity.create", R"({"id": "player", "components": {"Transform2D": {}}})");
    const json::Value result = engine.ok("component.set",
                                         R"({"id": "player", "components": {
            "Transform2D": {"position": [0, 0], "rotation": 2},
            "Health": {"max": 100}}})");
    CHECK(result == parse(R"({"id": "player", "added": ["Health"],
                              "changed": {"Transform2D": {"rotation": 2}}})"));

    const json::Value removed =
        engine.ok("component.remove", R"({"id": "player", "components": ["Health", "Follow"]})");
    CHECK(*removed.find("removed") == parse(R"(["Health"])"));
}

TEST_CASE("world.query filters by components and field values") {
    Engine engine;
    engine.ok("batch", R"({"commands": [
        {"command": "entity.create", "params": {"id": "a", "components": {
            "Transform2D": {"position": [0, 150]}, "Health": {"current": 5}}}},
        {"command": "entity.create", "params": {"id": "b", "components": {
            "Transform2D": {"position": [0, 50]}, "Health": {}}}},
        {"command": "entity.create", "params": {"id": "c", "components": {
            "Transform2D": {"position": [10, 200]}, "Follow": {"target": "a", "mode": "flee"}}}},
        {"command": "entity.create", "params": {"id": "d"}}
    ]})");

    const auto ids = [&](std::string_view params) {
        return *engine.ok("world.query", params).find("entities");
    };
    CHECK(ids("{}") == parse(R"(["a", "b", "c", "d"])"));
    CHECK(ids(R"({"with": ["Health"]})") == parse(R"(["a", "b"])"));
    CHECK(ids(R"({"with": ["Transform2D"], "without": ["Health"]})") == parse(R"(["c"])"));
    CHECK(ids(R"({"where": [{"component": "Transform2D", "field": "position.y", "op": ">",
                             "value": 100}]})") == parse(R"(["a", "c"])"));
    CHECK(ids(R"({"where": [{"component": "Transform2D", "field": "position.1", "op": "<=",
                             "value": 150},
                            {"component": "Health", "field": "current", "op": "<",
                             "value": 10}]})") == parse(R"(["a"])"));
    CHECK(ids(R"({"where": [{"component": "Follow", "field": "target", "value": "a"}]})") ==
          parse(R"(["c"])"));
    CHECK(ids(R"({"where": [{"component": "Follow", "field": "mode", "op": "!=",
                             "value": "chase"}]})") == parse(R"(["c"])"));

    const json::Value limited = engine.ok("world.query", R"({"limit": 2})");
    CHECK(*limited.find("count") == json::Value(4));
    CHECK(limited.find("truncated")->as_bool());
    CHECK(limited.find("entities")->size() == 2);

    const json::Value included = engine.ok(
        "world.query", R"({"with": ["Follow"], "include": ["Follow"], "omit_defaults": true})");
    CHECK(*included.find("entities") ==
          parse(R"([{"id": "c", "components": {"Follow": {"mode": "flee", "target": "a"}}}])"));
    const json::Value everything = engine.ok("world.query", R"({"with": ["Follow"],
                                                                 "include": true})");
    CHECK(everything.find("entities")->as_array().front().find("components")->size() == 2);

    auto reply = engine.fails("world.query", R"({"where": [{"component": "Transform2D",
                                  "field": "positon.y", "value": 1}]})",
                              command::code::kInvalidParams);
    CHECK(reply.data.find("path")->as_string() == "/where/0/field");
    CHECK(contains(reply.message, "did you mean \"position\""));
    engine.fails("world.query", R"({"where": [{"component": "Health", "field": "current",
                                    "op": "<", "value": [1]}]})",
                 command::code::kInvalidParams);
    engine.fails("world.query", R"({"with": ["Helth"]})", command::code::kNotFound);
}

TEST_CASE("undo and redo walk the history") {
    Engine engine;
    engine.ok("entity.create", R"({"id": "player", "components": {"Health": {}}})");
    engine.ok("component.set", R"({"id": "player", "components": {"Health": {"current": 10}}})");
    CHECK(engine.session->undo_labels() ==
          std::vector<std::string>{"entity.create", "component.set"});

    CHECK(engine.ok("undo") == parse(R"({"undone": "component.set", "can_undo": 1,
                                         "can_redo": 1})"));
    CHECK(engine.scene() == parse(R"({"player": {"components": {"Health": {}}}})"));
    engine.ok("undo");
    CHECK(engine.session->world().size() == 0);
    engine.fails("undo", "{}", command::code::kInvalidArgument);

    engine.ok("redo");
    engine.ok("redo");
    CHECK(engine.scene() == parse(R"({"player": {"components": {"Health": {"current": 10}}}})"));
    engine.fails("redo", "{}", command::code::kInvalidArgument);

    engine.ok("undo");
    CHECK(engine.ok("history.get") ==
          parse(R"({"undo": ["entity.create"], "redo": ["component.set"]})"));
    engine.ok("entity.destroy", R"({"id": "player"})");
    CHECK(engine.session->redo_labels().empty());

    // Reads leave no history.
    engine.ok("world.query");
    CHECK(engine.session->undo_labels().size() == 2);
}

TEST_CASE("history keeps at most history_limit steps") {
    command::SessionOptions options;
    options.history_limit = 2;
    Engine engine(options);
    engine.ok("entity.create", R"({"id": "a"})");
    engine.ok("entity.create", R"({"id": "b"})");
    engine.ok("entity.create", R"({"id": "c"})");
    CHECK(engine.session->undo_labels().size() == 2);
}

TEST_CASE("batch runs commands as one all-or-nothing step") {
    Engine engine;
    const json::Value result = engine.ok("batch", R"({"commands": [
        {"command": "entity.create", "params": {"id": "a"}},
        {"command": "component.set", "params": {"id": "a", "components": {"Health": {}}}},
        {"command": "entity.get", "params": {"id": "a", "omit_defaults": true}}
    ]})");
    CHECK(*result.find("results") == parse(R"([
        {"id": "a"},
        {"id": "a", "added": ["Health"], "changed": {}},
        {"id": "a", "components": {"Health": {}}}
    ])"));
    CHECK(engine.session->undo_labels() == std::vector<std::string>{"batch"});

    const auto reply = engine.fails("batch", R"({"commands": [
        {"command": "entity.create", "params": {"id": "b"}},
        {"command": "component.set", "params": {"id": "b", "components": {
            "Health": {"curent": 1}}}}
    ]})",
                                    command::code::kInvalidParams);
    CHECK(reply.data.find("index")->as_int() == 1);
    CHECK(reply.data.find("command")->as_string() == "component.set");
    CHECK(reply.data.find("path")->as_string() == "/commands/1/params/components/Health/curent");
    CHECK((engine.session->world().find("b") == entt::null));
    CHECK(engine.session->undo_labels().size() == 1);

    engine.ok("undo");
    CHECK(engine.session->world().size() == 0);

    // Read-only batches leave no history.
    engine.ok("batch", R"({"commands": [{"command": "world.query"}]})");
    CHECK(engine.session->undo_labels().empty());

    engine.fails("batch", R"({"commands": [{"command": "undo"}]})", command::code::kInvalidParams);
    engine.fails("batch", R"({"commands": [{"command": "batch", "params": {"commands": []}}]})",
                 command::code::kInvalidParams);
    const auto unknown = engine.fails("batch", R"({"commands": [{"command": "entity.creat"}]})",
                                      command::code::kInvalidParams);
    CHECK(contains(unknown.message, "did you mean \"entity.create\""));
}

TEST_CASE("dry runs report what would change and change nothing") {
    Engine engine;
    engine.ok("batch", R"({"commands": [
        {"command": "entity.create", "params": {"id": "player",
            "components": {"Transform2D": {"position": [1, 2]}, "Health": {}}}},
        {"command": "entity.create", "params": {"id": "enemy"}}]})");
    const json::Value before = engine.scene();
    const std::vector<std::string> history = engine.session->undo_labels();

    CHECK(engine.ok("component.set", R"({"id": "player", "dry_run": true,
            "components": {"Transform2D": {"rotation": 1.5}}})") == parse(R"({
        "dry_run": true,
        "result": {"id": "player", "changed": {"Transform2D": {"rotation": 1.5}}},
        "changes": {"changed": {"player": {"Transform2D": {"rotation": 1.5}}}}
    })"));
    CHECK(engine.scene() == before);
    CHECK(engine.session->undo_labels() == history);

    const json::Value batch = engine.ok("batch", R"({"dry_run": true, "commands": [
        {"command": "entity.create", "params": {"components": {"Transform2D": {}}}},
        {"command": "entity.destroy", "params": {"id": "enemy"}},
        {"command": "component.remove", "params": {"id": "player", "components": ["Health"]}},
        {"command": "component.set", "params": {"id": "player",
            "components": {"Follow": {"mode": "flee"}}}}]})");
    CHECK(*batch.find("changes") == parse(R"({
        "added": ["entity_1"],
        "removed": ["enemy"],
        "changed": {"entity_1": {"Transform2D": {}},
                    "player": {"Follow": {"mode": "flee"}, "Health": null}}
    })"));
    CHECK(engine.scene() == before);
    CHECK(engine.session->undo_labels() == history);

    // The next generated ID and the tick counter are as if the dry runs never happened.
    CHECK(engine.ok("entity.create").find("id")->as_string() == "entity_1");
    CHECK(engine.ok("step", R"({"ticks": 5, "dry_run": true})")
              .find("result")
              ->find("tick")
              ->as_int() == 5);
    CHECK(engine.ok("step").find("tick")->as_int() == 1);

    // Replacing the scene: everything would go, and the undo history stays.
    CHECK(*engine.ok("scene.new", R"({"dry_run": true})").find("changes") ==
          parse(R"({"removed": ["enemy", "entity_1", "player"]})"));
    CHECK(engine.scene().size() == 3);
    CHECK(engine.session->undo_labels().size() == history.size() + 2);

    // Errors are reported as usual, and dry_run false is an ordinary call.
    engine.fails("entity.destroy", R"({"id": "nobody", "dry_run": true})",
                 command::code::kNotFound);
    engine.ok("entity.destroy", R"({"id": "enemy", "dry_run": false})");
    CHECK(engine.scene().find("enemy") == nullptr);

    // dry_run goes on the batch, and only commands that change the world take it.
    const command::Reply nested = engine.fails(
        "batch", R"({"commands": [{"command": "entity.create", "params": {"dry_run": true}}]})",
        command::code::kInvalidParams);
    CHECK(nested.data.find("path")->as_string() == "/commands/0/params/dry_run");
    engine.fails("entity.get", R"({"id": "player", "dry_run": true})",
                 command::code::kInvalidParams);
    const json::Value schema = engine.ok("schema.get", R"({"kind": "command", "name": "batch"})");
    CHECK(schema.find("properties")->find("dry_run") != nullptr);
}

TEST_CASE("world snapshots show what changed and restore it") {
    Engine engine;
    engine.ok("entity.create",
              R"({"id": "player", "components": {"Transform2D": {"position": [1, 2]}}})");
    const command::Reply none = engine.fails("world.diff", "{}", command::code::kNotFound);
    CHECK(none.message == "/from: no snapshot is named \"default\"; take one with "
                          "world.snapshot first");
    CHECK(engine.ok("world.snapshot") == parse(R"({"name": "default", "entities": 1})"));
    CHECK(engine.ok("world.diff") == parse("{}"));

    engine.ok("component.set",
              R"({"id": "player", "components": {"Transform2D": {"position": [5, 2]}}})");
    engine.ok("entity.create", R"({"id": "coin"})");
    CHECK(engine.ok("world.diff") == parse(R"({
        "added": ["coin"],
        "changed": {"player": {"Transform2D": {"position": [5, 2]}}}
    })"));

    engine.ok("world.snapshot", R"({"name": "moved"})");
    CHECK(engine.ok("world.diff", R"({"from": "moved", "to": "default"})") == parse(R"({
        "removed": ["coin"],
        "changed": {"player": {"Transform2D": {"position": [1, 2]}}}
    })"));
    const command::Reply typo =
        engine.fails("world.restore", R"({"name": "moev"})", command::code::kNotFound);
    CHECK(typo.message == "/name: no snapshot is named \"moev\"; did you mean \"moved\"?");
    CHECK(typo.data.find("path")->as_string() == "/name");

    // A dry run shows what restoring would change; restoring is one undo step.
    CHECK(*engine.ok("world.restore", R"({"dry_run": true})").find("changes") ==
          engine.ok("world.diff", R"({"from": "moved", "to": "default"})"));
    CHECK(engine.ok("world.restore") == parse(R"({"restored": "default", "entities": 1})"));
    CHECK(engine.ok("world.diff") == parse("{}"));
    CHECK(engine.session->undo_labels().back() == "world.restore");
    engine.ok("undo");
    CHECK(engine.ok("world.diff", R"({"from": "moved"})") == parse("{}"));
}

TEST_CASE("events.subscribe queues entity, command and game events") {
    Engine engine;
    CHECK(engine.fails("events.read", R"({"subscription": "events_1"})", command::code::kNotFound)
              .message == "/subscription: no subscription is named \"events_1\"; start one with "
                          "events.subscribe");
    CHECK(engine.ok("events.subscribe") == parse(R"({"subscription": "events_1"})"));
    CHECK(engine.ok("events.subscribe",
                    R"({"kinds": ["entity.created", "entity.destroyed"], "ids": ["coin"]})") ==
          parse(R"({"subscription": "events_2"})"));

    engine.ok("entity.create", R"({"id": "coin", "components": {"Transform2D": {}}})");
    engine.ok("entity.create", R"({"id": "wall"})");
    engine.ok("component.set", R"({"id": "coin", "components": {"Transform2D": {"rotation": 1}}})");
    // Gameplay code emits its own events.
    engine.session->world().add_system("pickup", [](world::World& w, double /*seconds*/) {
        if (const entt::entity coin = w.find("coin"); coin != entt::null) {
            w.emit("coin_collected", "coin", parse(R"({"value": 5})"));
            w.destroy(coin);
        }
    });
    engine.ok("step");
    // Failed commands and dry runs change nothing, so they queue nothing.
    engine.fails("entity.create", R"({"id": "wall"})", command::code::kInvalidArgument);
    engine.ok("entity.create", R"({"id": "ghost", "dry_run": true})");

    CHECK(engine.ok("events.read", R"({"subscription": "events_1"})") == parse(R"({
        "dropped": 0, "more": false, "events": [
        {"seq": 1, "tick": 0, "kind": "entity.created", "id": "coin"},
        {"seq": 2, "tick": 0, "kind": "command", "command": "entity.create",
         "changes": {"added": ["coin"], "changed": {"coin": {"Transform2D": {}}}}},
        {"seq": 3, "tick": 0, "kind": "entity.created", "id": "wall"},
        {"seq": 4, "tick": 0, "kind": "command", "command": "entity.create",
         "changes": {"added": ["wall"]}},
        {"seq": 5, "tick": 0, "kind": "command", "command": "component.set",
         "changes": {"changed": {"coin": {"Transform2D": {"rotation": 1}}}}},
        {"seq": 6, "tick": 0, "kind": "coin_collected", "id": "coin", "data": {"value": 5}},
        {"seq": 7, "tick": 0, "kind": "entity.destroyed", "id": "coin"},
        {"seq": 8, "tick": 1, "kind": "command", "command": "step",
         "changes": {"removed": ["coin"]}}
    ]})"));
    // The filtered subscription saw only the coin coming and going.
    const json::Value coin = engine.ok("events.read", R"({"subscription": "events_2"})");
    REQUIRE(coin.find("events")->as_array().size() == 2);
    CHECK(coin.find("events")->as_array()[1] ==
          parse(R"({"seq": 7, "tick": 0, "kind": "entity.destroyed", "id": "coin"})"));
    CHECK(engine.ok("events.read", R"({"subscription": "events_1"})").find("events")->size() == 0);

    // Undo reports what it put back; a new scene reports one change, not every entity.
    engine.ok("undo");
    engine.ok("scene.new");
    const json::Value later =
        engine.ok("events.read", R"({"subscription": "events_1", "limit": 1})");
    CHECK(later.find("more")->as_bool());
    CHECK(*later.find("events") == parse(R"([{"seq": 9, "tick": 1, "kind": "command",
        "command": "undo", "changes": {"added": ["coin"],
        "changed": {"coin": {"Transform2D": {"rotation": 1}}}}}])"));
    CHECK(*engine.ok("events.read", R"({"subscription": "events_1"})").find("events") ==
          parse(R"([{"seq": 10, "tick": 1, "kind": "command", "command": "scene.new",
              "changes": {"removed": ["coin", "wall"]}}])"));

    // A full queue drops the oldest events and says how many.
    const std::string created = engine.ok("events.subscribe", R"({"kinds": ["entity.created"]})")
                                    .find("subscription")
                                    ->as_string();
    json::Array creates;
    for (int i = 0; i <= static_cast<int>(command::Session::kMaxQueuedEvents); ++i) {
        json::Value item;
        item["command"] = "entity.create";
        creates.push_back(std::move(item));
    }
    json::Value batch;
    batch["commands"] = json::Array(creates.begin(), creates.begin() + 1000);
    REQUIRE(engine.session->call("batch", batch).ok);
    REQUIRE(engine.session->call("entity.create", json::Value()).ok);
    const json::Value full =
        engine.ok("events.read", R"({"subscription": ")" + created + R"(", "limit": 1000})");
    CHECK(full.find("dropped")->as_int() == 1);
    CHECK(full.find("events")->as_array().front().find("id")->as_string() == "entity_2");

    CHECK(engine.ok("events.unsubscribe", R"({"subscription": "events_2"})") ==
          parse(R"({"unsubscribed": "events_2"})"));
    CHECK(engine.fails("events.read", R"({"subscription": "event_1"})", command::code::kNotFound)
              .message == "/subscription: no subscription is named \"event_1\"; did you mean "
                          "\"events_1\"?");
    CHECK(engine
              .fails("events.subscribe", R"({"kinds": ["entity.create"]})",
                     command::code::kInvalidParams)
              .message == "/kinds/0: unknown event kind \"entity.create\"; did you mean "
                          "\"entity.created\"?");
}

TEST_CASE("sessions only run commands their capabilities allow") {
    command::SessionOptions options;
    options.capabilities = {Capability::Read};
    Engine engine(options);
    engine.ok("world.query");
    const auto reply = engine.fails("entity.create", "{}", command::code::kPermissionDenied);
    CHECK(reply.data.find("capability")->as_string() == "edit");
    engine.fails("batch", R"({"commands": [{"command": "step"}]})",
                 command::code::kPermissionDenied);
    engine.fails("scene.save", R"({"path": "a.scene.json"})", command::code::kPermissionDenied);
}

TEST_CASE("paths stay inside the project folder") {
    command::SessionOptions options;
    options.project_root = "game";
    Engine engine(options);
    const command::Session& session = *engine.session;
    CHECK(session.resolve_path("scenes/main.scene.json").value() == "game/scenes/main.scene.json");
    CHECK(session.resolve_path("scenes\\a.scene.json").value() == "game/scenes/a.scene.json");
    for (const std::string_view bad :
         {"../secret.scene.json", "a/../../b.scene.json", "/etc/passwd", "\\\\server\\share",
          "C:/game/a.scene.json", ".."}) {
        CHECK_MESSAGE(session.resolve_path(bad).error().code == slash::ErrorCode::PermissionDenied,
                      bad);
    }
    CHECK_FALSE(session.resolve_path("").ok());
    CHECK(session.resolve_path("a..b/c.scene.json").ok());
    // Windows opens a device for these names, so no project uses them.
    CHECK(session.resolve_path("levels/aux.scene.json").error().code ==
          slash::ErrorCode::InvalidArgument);
    const command::Reply saved = engine.fails("scene.save", R"({"path": "levels/aux.scene.json"})",
                                              command::code::kInvalidArgument);
    CHECK(contains(saved.message, "a name Windows keeps for a device"));
}

TEST_CASE("scenes save to and open from the project folder") {
    const std::string root = "command_test_project";
    REQUIRE(slash::platform::create_directories(root).ok());
    command::SessionOptions options;
    options.project_root = root;
    Engine engine(options);

    engine.fails("scene.save", "{}", command::code::kInvalidArgument);
    engine.fails("scene.save", R"({"path": "level.json"})", command::code::kInvalidArgument);

    engine.ok("entity.create", R"({"id": "player", "components": {"Health": {"current": 7}}})");
    CHECK(engine.ok("scene.save", R"({"path": "levels/one/main.scene.json"})") ==
          parse(R"({"path": "levels/one/main.scene.json", "entities": 1})"));
    CHECK(engine.session->scene_path() == "levels/one/main.scene.json");
    const auto text = slash::platform::read_file(root + "/levels/one/main.scene.json");
    REQUIRE(text.ok());
    CHECK(parse(text.value()) == parse(R"({"entities": {"player": {"components": {
                                               "Health": {"current": 7}}}}, "format": 1})"));

    engine.ok("scene.new", R"({"path": "levels/two.scene.json"})");
    CHECK(engine.session->world().size() == 0);
    CHECK(engine.session->undo_labels().empty());
    CHECK(engine.ok("scene.save").find("path")->as_string() == "levels/two.scene.json");

    engine.ok("entity.create", R"({"id": "temp"})");
    CHECK(engine.ok("scene.open", R"({"path": "levels/one/main.scene.json"})") ==
          parse(R"({"path": "levels/one/main.scene.json", "entities": 1})"));
    CHECK((engine.session->world().find("player") != entt::null));
    CHECK(engine.session->undo_labels().empty());

    REQUIRE(slash::platform::write_file(root + "/bad.scene.json",
                                        R"({"entities": {"x": {"components": {
                                            "Health": {"curent": 1}, "Nope": {}}}}, "format": 1})")
                .ok());
    const auto reply = engine.fails("scene.open", R"({"path": "bad.scene.json"})",
                                    command::code::kInvalidArgument);
    CHECK(reply.data.find("file_errors")->size() == 2);
    CHECK((engine.session->world().find("player") != entt::null));
    CHECK(engine.session->scene_path() == "levels/one/main.scene.json");

    engine.fails("scene.open", R"({"path": "missing.scene.json"})", command::code::kNotFound);

    // --scene opens a file that exists, or starts a new scene that saves there.
    CHECK(command::open_or_new_scene(*engine.session, "levels/two.scene.json").ok);
    CHECK(command::open_or_new_scene(*engine.session, "levels/three.scene.json").ok);
    CHECK(engine.session->scene_path() == "levels/three.scene.json");
    CHECK(engine.session->world().size() == 0);
}

TEST_CASE("entities instance scenes, which bring their entities") {
    const std::string root = "command_instance_project";
    std::filesystem::remove_all(root);
    REQUIRE(slash::platform::create_directories(root).ok());
    command::SessionOptions options;
    options.project_root = root;
    Engine engine(options);

    // A player scene, saved with the entity its instances become.
    engine.ok("scene.new", R"({"path": "player.scene.json"})");
    engine.ok("entity.create", R"({"id": "sprite", "components": {
        "Transform2D": {"position": [0, -8]}}})");
    engine.ok("entity.create", R"({"id": "player", "components": {
        "Transform2D": {"children": ["sprite"]}, "Health": {"max": 50}}})");
    engine.fails("scene.save", R"({"root": "nobody"})", command::code::kNotFound);
    CHECK(engine.ok("scene.save", R"({"root": "player"})") ==
          parse(R"({"path": "player.scene.json", "entities": 2, "root": "player"})"));
    CHECK(engine.ok("scene.save") ==
          parse(R"({"path": "player.scene.json", "entities": 2, "root": "player"})"));

    engine.ok("scene.new", R"({"path": "main.scene.json"})");
    engine.ok("entity.create", R"({"id": "hero", "extends": "player.scene.json",
                                   "components": {"Transform2D": {"position": [10, 0]}}})");
    CHECK(engine.ok("entity.get", R"({"id": "hero/sprite", "omit_defaults": true})") == parse(R"({
        "id": "hero/sprite", "components": {"Transform2D": {"position": [0.0, -8.0]}},
        "parent": "hero", "global": {"position": [10, -8], "rotation": 0, "scale": [1, 1]}})"));
    engine.ok("component.set", R"({"id": "hero/sprite", "components": {
        "Transform2D": {"position": [0, -10]}}})");
    CHECK(engine.scene() == parse(R"({
        "hero": {"extends": "player.scene.json",
                 "components": {"Transform2D": {"position": [10.0, 0.0]}}},
        "hero/sprite": {"components": {"Transform2D": {"position": [0.0, -10.0]}}}})"));

    // IDs with '/' belong to instances, and prefab files hold entities of a scene's own.
    engine.fails("entity.create", R"({"id": "hero/hat"})", command::code::kInvalidArgument);
    engine.fails("prefab.save", R"({"id": "hero", "path": "hero.prefab.json"})",
                 command::code::kInvalidArgument);
    engine.fails("prefab.save", R"({"id": "hero/sprite", "path": "sprite.prefab.json"})",
                 command::code::kInvalidArgument);
    engine.fails("scene.save", R"({"root": "hero/sprite"})", command::code::kInvalidArgument);

    // Editing the scene file updates the instance before the next command.
    REQUIRE(slash::platform::write_file(root + "/player.scene.json", R"({"format": 1,
        "root": "player", "entities": {
            "player": {"components": {"Transform2D": {"children": ["sprite", "shadow"]}}},
            "shadow": {"components": {"Transform2D": {"position": [0, 4]}}},
            "sprite": {"components": {"Transform2D": {"position": [0, -8]}}}}})")
                .ok());
    CHECK(*engine.ok("world.query", R"({"with": ["Transform2D"]})").find("entities") ==
          parse(R"(["hero", "hero/shadow", "hero/sprite"])"));

    // Destroying the instance takes what it brought; undo puts it all back.
    const json::Value destroyed = engine.ok("entity.destroy", R"({"id": "hero"})");
    CHECK(destroyed ==
          parse(R"({"destroyed": "hero", "descendants": ["hero/sprite", "hero/shadow"]})"));
    CHECK(engine.session->world().size() == 0);
    engine.ok("undo");
    CHECK(engine.scene() == parse(R"({
        "hero": {"extends": "player.scene.json",
                 "components": {"Transform2D": {"position": [10.0, 0.0]}}},
        "hero/sprite": {"components": {"Transform2D": {"position": [0.0, -10.0]}}}})"));
}

TEST_CASE("entities extend prefabs, and prefab.save writes them") {
    const std::string root = "command_prefab_project";
    std::filesystem::remove_all(root);
    REQUIRE(slash::platform::create_directories(root + "/prefabs").ok());
    REQUIRE(slash::platform::write_file(root + "/prefabs/enemy.prefab.json",
                                        R"({"format": 1, "components": {
                                            "Health": {"current": 50, "max": 50},
                                            "Transform2D": {"rotation": 1}}})")
                .ok());
    command::SessionOptions options;
    options.project_root = root;
    Engine engine(options);

    engine.ok("entity.create", R"({"id": "grunt", "extends": "prefabs/enemy.prefab.json",
                                   "components": {"Health": {"current": 20}}})");
    CHECK(engine.ok("entity.get", R"({"id": "grunt", "components": ["Health"]})") == parse(R"({
        "id": "grunt", "extends": "prefabs/enemy.prefab.json",
        "components": {"Health": {"current": 20.0, "max": 50.0}}})"));
    CHECK(engine.scene() == parse(R"({"grunt": {"extends": "prefabs/enemy.prefab.json",
                                                "components": {"Health": {"current": 20.0}}}})"));
    const auto missing =
        engine.fails("entity.create", R"({"id": "x", "extends": "prefabs/gone.prefab.json"})",
                     command::code::kNotFound);
    CHECK(*missing.data.find("path") == "/extends");
    CHECK((engine.session->world().find("x") == entt::null));

    // Editing a prefab file updates its instances before the next command, keeping their
    // overrides: grunt keeps its own current health and loses the Transform2D the prefab dropped.
    REQUIRE(slash::platform::write_file(root + "/prefabs/enemy.prefab.json",
                                        R"({"format": 1, "components": {
                                            "Health": {"current": 80, "max": 80}}})")
                .ok());
    engine.ok("entity.create", R"({"id": "fresh", "extends": "prefabs/enemy.prefab.json"})");
    CHECK(engine.ok("entity.get", R"({"id": "fresh"})").find("components")->size() == 1);
    const json::Value reloaded = parse(R"({
        "id": "grunt", "extends": "prefabs/enemy.prefab.json",
        "components": {"Health": {"current": 20.0, "max": 80.0}}})");
    CHECK(engine.ok("entity.get", R"({"id": "grunt", "omit_defaults": true})") == reloaded);
    CHECK(engine.scene().find("grunt")->find("components")->size() == 1);

    // Undo restores instances with the prefab they had, without reading the file.
    std::filesystem::remove(root + "/prefabs/enemy.prefab.json");
    engine.ok("entity.destroy", R"({"id": "grunt"})");
    engine.ok("undo");
    CHECK(engine.ok("entity.get", R"({"id": "grunt", "omit_defaults": true})") == reloaded);
    CHECK(engine.ok("asset.list", R"({"type": "prefab"})") == parse(R"({"assets": [
        {"path": "prefabs/enemy.prefab.json", "type": "prefab", "refs": 2, "version": 3}]})"));
    const json::Value dry = engine.ok("component.set", R"({"id": "grunt", "dry_run": true,
        "components": {"Health": {"max": 60}}})");
    CHECK(*dry.find("changes") == parse(R"({"changed": {"grunt": {"Health": {"max": 60.0}}}})"));

    // prefab.save writes an entity as a prefab, and later commands read it.
    engine.ok("entity.create", R"({"id": "crate", "components": {"Health": {"max": 5}}})");
    CHECK(engine.ok("prefab.save", R"({"id": "crate", "path": "prefabs/crate.prefab.json"})") ==
          parse(R"({"path": "prefabs/crate.prefab.json", "components": ["Health"]})"));
    const auto text = slash::platform::read_file(root + "/prefabs/crate.prefab.json");
    REQUIRE(text.ok());
    CHECK(parse(text.value()) == parse(R"({"components": {"Health": {"max": 5.0}}, "format": 1})"));
    engine.ok("batch", R"({"commands": [
        {"command": "component.set", "params": {"id": "crate",
                                               "components": {"Health": {"max": 9}}}},
        {"command": "prefab.save", "params": {"id": "crate", "path": "prefabs/crate.prefab.json"}},
        {"command": "entity.create", "params": {"id": "crate_2",
                                               "extends": "prefabs/crate.prefab.json"}}]})");
    CHECK(engine.ok("entity.get", R"({"id": "crate_2", "components": ["Health"]})") ==
          parse(R"({"id": "crate_2", "extends": "prefabs/crate.prefab.json",
                    "components": {"Health": {"current": 100.0, "max": 9.0}}})"));

    // In a dry run, file-writing commands write nothing, but the rest of the batch sees the
    // prefab.
    const json::Value dry_batch = engine.ok("batch", R"({"dry_run": true, "commands": [
        {"command": "prefab.save", "params": {"id": "crate", "path": "prefabs/box.prefab.json"}},
        {"command": "entity.create", "params": {"id": "box", "extends": "prefabs/box.prefab.json"}},
        {"command": "scene.save", "params": {"path": "dry.scene.json"}}]})");
    CHECK(*dry_batch.find("changes") ==
          parse(R"({"added": ["box"], "changed": {"box": {"Health": {"max": 9.0}}}})"));
    CHECK_FALSE(std::filesystem::exists(root + "/prefabs/box.prefab.json"));
    CHECK_FALSE(std::filesystem::exists(root + "/dry.scene.json"));

    // A prefab cannot extend itself.
    const auto loop =
        engine.fails("prefab.save", R"({"id": "crate_2", "path": "prefabs/crate.prefab.json"})",
                     command::code::kInvalidArgument);
    CHECK(contains(loop.message, "loop"));
    engine.fails("prefab.save", R"({"id": "crate", "path": "prefabs/crate.json"})",
                 command::code::kInvalidArgument);
    engine.fails("prefab.save", R"({"id": "crate", "path": "../crate.prefab.json"})",
                 command::code::kPermissionDenied);

    // Scenes save overrides only and open with their prefabs.
    engine.ok("entity.destroy", R"({"id": "grunt"})");
    engine.ok("entity.destroy", R"({"id": "fresh"})");
    engine.ok("scene.save", R"({"path": "main.scene.json"})");
    engine.ok("scene.new");
    CHECK(engine.ok("scene.open", R"({"path": "main.scene.json"})").find("entities")->as_int() ==
          2);
    CHECK(engine.ok("entity.get", R"({"id": "crate_2", "components": ["Health"]})")
              .find("components")
              ->find("Health")
              ->find("max")
              ->as_double() == 9.0);
    std::filesystem::remove_all(root);
}

TEST_CASE("changed files reload, and prefab instances update with their overrides") {
    const std::string root = "command_reload_project";
    std::filesystem::remove_all(root);
    REQUIRE(slash::platform::create_directories(root + "/prefabs").ok());
    REQUIRE(slash::platform::create_directories(root + "/data").ok());
    REQUIRE(slash::platform::write_file(root + "/prefabs/enemy.prefab.json",
                                        R"({"format": 1, "components": {"Health": {"max": 50}}})")
                .ok());
    REQUIRE(slash::platform::write_file(root + "/data/tuning.json", R"({"speed": 3})").ok());
    command::SessionOptions options;
    options.project_root = root;
    Engine engine(options);

    // Gameplay code loads assets through the session's asset store.
    auto tuning = engine.session->assets().load<json::Value>("data/tuning.json");
    REQUIRE(tuning.ok());
    engine.ok("entity.create", R"({"id": "grunt", "extends": "prefabs/enemy.prefab.json",
                                   "components": {"Health": {"current": 20}}})");
    engine.ok("events.subscribe", R"({"kinds": ["asset.reloaded", "command"]})");

    REQUIRE(slash::platform::write_file(root + "/prefabs/enemy.prefab.json",
                                        R"({"format": 1, "components": {"Health": {"max": 90},
                                            "Transform2D": {"rotation": 2}}})")
                .ok());
    REQUIRE(slash::platform::write_file(root + "/data/tuning.json", R"({"speed": 70})").ok());
    CHECK(engine.ok("asset.list") == parse(R"({"assets": [
        {"path": "data/tuning.json", "type": "json", "refs": 1, "version": 2},
        {"path": "prefabs/enemy.prefab.json", "type": "prefab", "refs": 1, "version": 2}]})"));
    CHECK(tuning.value()->find("speed")->as_int() == 70);
    CHECK(engine.ok("entity.get", R"({"id": "grunt", "omit_defaults": true})") == parse(R"({
        "id": "grunt", "extends": "prefabs/enemy.prefab.json",
        "components": {"Health": {"current": 20.0, "max": 90.0},
                       "Transform2D": {"rotation": 2.0}}})"));
    CHECK(engine.ok("events.read", R"({"subscription": "events_1"})") == parse(R"({
        "dropped": 0, "more": false, "events": [
        {"seq": 1, "tick": 0, "kind": "asset.reloaded",
         "data": {"path": "data/tuning.json", "type": "json", "version": 2}},
        {"seq": 2, "tick": 0, "kind": "asset.reloaded",
         "data": {"path": "prefabs/enemy.prefab.json", "type": "prefab", "version": 2}},
        {"seq": 3, "tick": 0, "kind": "command", "command": "asset.reload",
         "changes": {"changed": {"grunt": {"Health": {"max": 90.0},
                                           "Transform2D": {"rotation": 2.0}}}}}
    ]})"));
    // The reload is not an undo step: undo reverts the entity.create before it.
    CHECK(engine.session->undo_labels() == std::vector<std::string>{"entity.create"});

    // A prefab that no longer loads leaves its instances as they are.
    REQUIRE(
        slash::platform::write_file(root + "/prefabs/enemy.prefab.json", "{\"format\": 1,").ok());
    const json::Value events = engine.ok("events.read", R"({"subscription": "events_1"})");
    REQUIRE(events.find("events")->size() == 1);
    const json::Value& failed = *events.find("events")->as_array()[0].find("data");
    CHECK(failed.find("path")->as_string() == "prefabs/enemy.prefab.json");
    CHECK(contains(failed.find("error")->as_string(), "is not valid"));
    CHECK(engine.ok("entity.get", R"({"id": "grunt", "components": ["Health"]})")
              .find("components")
              ->find("Health")
              ->find("max")
              ->as_double() == 90.0);
    std::filesystem::remove_all(root);
}

TEST_CASE("storyboard.edit writes entries, refuses stale versions and undoes") {
    const std::string root = "command_storyboard_project";
    std::filesystem::remove_all(root);
    command::SessionOptions options;
    options.project_root = root;
    Engine engine(options);
    const auto read = [&](const std::string& path) {
        auto text = slash::platform::read_file(root + "/" + path);
        REQUIRE(text.ok());
        return std::move(text).value();
    };

    const json::Value created = engine.ok("storyboard.edit", R"({"why": "first cast", "edits": [
        {"id": "faction/couriers", "version": null, "set": {"name": "The Couriers"}},
        {"id": "character/mira", "version": null,
         "set": {"name": "Mira", "faction": "faction/couriers"}}]})");
    const std::string mira = created.find("changes")->find("character/mira")->as_string();
    CHECK(created.find("seq")->as_int() == 1);
    CHECK(read("storyboard/world.json") == R"({
  "entries": {
    "character/mira": {
      "faction": "faction/couriers",
      "name": "Mira"
    },
    "faction/couriers": {
      "name": "The Couriers"
    }
  },
  "format": 1
}
)");
    const json::Value line = parse(read("storyboard/journal.jsonl"));
    CHECK(line.find("by")->as_string() == "unknown");
    CHECK(line.find("why")->as_string() == "first cast");
    CHECK(*line.find("changes") == *created.find("changes"));

    CHECK(engine.ok("storyboard.query", R"({"kind": "character"})") == parse(R"({"seq": 1,
        "entries": [{"id": "character/mira", "version": ")" + mira + R"(",
                     "fields": {"name": "Mira"}}]})"));
    engine.fails("storyboard.query", R"({"ids": ["character/mra"]})", command::code::kNotFound);

    // Another agent's change makes a version stale; the error carries the entry as it is now.
    const auto stale = engine.fails("storyboard.edit", R"({"edits": [{"id": "character/mira",
        "version": "0000000a", "set": {"voice": "dry"}}]})",
                                    command::code::kConflict);
    CHECK(stale.data.find("current")->find("version")->as_string() == mira);
    engine.fails("storyboard.edit",
                 R"({"edits": [{"id": "character/mira", "version": ")" + mira +
                     R"(", "set": {"home": "place/nowhere"}}]})",
                 command::code::kInvalidParams);

    // A dry run, and a failed batch, write nothing.
    const std::string world_before = read("storyboard/world.json");
    const std::string journal_before = read("storyboard/journal.jsonl");
    const json::Value dry = engine.ok("storyboard.edit", R"({"dry_run": true, "edits": [
        {"id": "character/mira", "version": ")" + mira + R"(", "set": {"voice": "dry"}}]})");
    CHECK(dry.find("result")->find("seq")->as_int() == 2);
    engine.fails("batch", R"({"commands": [
        {"command": "storyboard.edit", "params": {"edits": [{"id": "place/docks",
                                                             "version": null,
                                                             "set": {"name": "Docks"}}]}},
        {"command": "entity.destroy", "params": {"id": "nobody"}}]})",
                 command::code::kNotFound);
    CHECK(read("storyboard/world.json") == world_before);
    CHECK(read("storyboard/journal.jsonl") == journal_before);

    // A batch that edits the world and the storyboard is one undo step.
    engine.ok("batch", R"({"commands": [
        {"command": "entity.create", "params": {"id": "courier"}},
        {"command": "storyboard.edit", "params": {"edits": [{"id": "place/docks",
                                                             "version": null,
                                                             "set": {"name": "Docks"}}]}}]})");
    CHECK(engine.session->undo_labels() == std::vector<std::string>{"storyboard.edit", "batch"});
    engine.ok("undo");
    CHECK(engine.scene().size() == 0);
    CHECK(read("storyboard/world.json") == world_before);
    engine.ok("redo");
    CHECK(engine.scene().size() == 1);
    CHECK(contains(read("storyboard/world.json"), "place/docks"));
    engine.ok("undo");

    // Undo of an edit that changed only files leaves the world alone, and is journaled.
    engine.ok("entity.create", R"({"id": "bystander"})");
    engine.session->clear_history();
    const json::Value voiced = engine.ok("storyboard.edit", R"({"edits": [{"id": "character/mira",
        "version": ")" + mira + R"(", "set": {"voice": "dry"}}]})");
    engine.ok("undo");
    CHECK(engine.scene().size() == 1);
    CHECK(read("storyboard/world.json") == world_before);
    const std::string journal = read("storyboard/journal.jsonl");
    CHECK(contains(journal, R"("why":"undo")"));
    engine.ok("redo");
    CHECK(contains(read("storyboard/world.json"), R"("voice": "dry")"));

    // Undo is refused, and changes nothing, once someone else changed the entry since.
    const std::string edited_elsewhere = R"({"format": 1, "entries": {
        "character/mira": {"name": "Mira", "voice": "warm", "faction": "faction/couriers"},
        "faction/couriers": {"name": "The Couriers"}}})";
    REQUIRE(slash::platform::write_file(root + "/storyboard/world.json", edited_elsewhere).ok());
    const auto refused = engine.fails("undo", "{}", command::code::kConflict);
    CHECK(contains(refused.message, "could not undo \"storyboard.edit\""));
    CHECK(read("storyboard/world.json") == edited_elsewhere);
    CHECK(engine.session->undo_labels() == std::vector<std::string>{"storyboard.edit"});
    CHECK(voiced.find("changes")->size() == 1);

    // An invalid storyboard file is reported, with every error, before any edit.
    REQUIRE(slash::platform::write_file(root + "/storyboard/story.json",
                                        R"({"format": 1, "entries": {"beat/x": {}}})")
                .ok());
    const auto invalid = engine.fails("storyboard.query", "{}", command::code::kInvalidArgument);
    CHECK(contains(invalid.message, "storyboard/story.json at /entries/beat~1x: missing required"));
    std::filesystem::remove_all(root);
}

TEST_CASE("storyboard.brief gathers the facts a task needs and storyboard.check finds mistakes") {
    const std::string root = "command_brief_project";
    std::filesystem::remove_all(root);
    REQUIRE(slash::platform::create_directories(root + "/build/export").ok());
    REQUIRE(slash::platform::create_directories(root + "/prefabs").ok());
    REQUIRE(slash::platform::create_directories(root + "/scenes").ok());
    const auto write = [&](const std::string& path, std::string_view text) {
        REQUIRE(slash::platform::write_file(root + "/" + path, text).ok());
    };
    write("prefabs/crate.prefab.json", R"({"components": {"StoryRef": {"ids": ["item/crate"]}},
        "format": 1})");
    write("scenes/docks.scene.json", R"({"entities": {
        "mira": {"components": {"StoryRef": {"ids": ["character/mira"]}}},
        "crate": {"extends": "prefabs/crate.prefab.json"}}, "format": 1})");
    write("scenes/roof.scene.json",
          R"({"entities": {"sign": {"components": {"StoryRef": {"ids": ["place/rooftop"]}}}},
              "format": 1})");
    // Exports copy scenes into build/; check leaves them to the project's own copy.
    write("build/export/roof.scene.json",
          R"({"entities": {"x": {"components": {"StoryRef": {"ids": ["place/nowhere"]}}}}})");
    command::SessionOptions options;
    options.project_root = root;
    Engine engine(options);
    const json::Value first = engine.ok("storyboard.edit", R"({"edits": [
        {"id": "character/mira", "version": null, "set": {"name": "Mira", "home": "place/roofs"}},
        {"id": "place/roofs", "version": null, "set": {"name": "The Roofs"}},
        {"id": "item/crate", "version": null,
         "set": {"name": "Crate", "links": ["prefabs/crate.prefab.json", "art/crate.png"]}},
        {"id": "rule/one_hit", "version": null, "set": {"name": "One hit"}},
        {"id": "term/couriers", "version": null, "set": {"name": "Couriers", "avoid": ["runners"]}},
        {"id": "task/voice", "version": null,
         "set": {"name": "Voice Mira", "refs": ["character/mira"], "status": "open"}}]})");
    const std::int64_t seq = first.find("seq")->as_int();

    const auto ids = [](const json::Value& result, std::string_view key) {
        std::vector<std::string> out;
        for (const json::Value& item : result.find(key)->as_array()) {
            out.push_back(item.find("id")->as_string());
        }
        return out;
    };
    // A scene brings its entities' StoryRef entries and the prefabs they extend.
    const json::Value docks =
        engine.ok("storyboard.brief", R"({"scenes": ["scenes/docks.scene.json"]})");
    CHECK(ids(docks, "entries") ==
          std::vector<std::string>{"character/mira", "item/crate", "rule/one_hit", "task/voice"});
    CHECK(ids(docks, "related") == std::vector<std::string>{"place/roofs"});
    CHECK(docks.find("seq")->as_int() == seq);
    CHECK(docks.find("changed") == nullptr);

    // Entities in the open scene count the same way, and since lists what changed after a seq.
    engine.ok("scene.open", R"({"path": "scenes/docks.scene.json"})");
    const std::string rule = first.find("changes")->find("rule/one_hit")->as_string();
    engine.ok("storyboard.edit",
              R"({"edits": [{"id": "rule/one_hit", "remove": true, "version": ")" + rule + "\"}]}");
    const json::Value mira = engine.ok("storyboard.brief", R"({"entities": ["mira"], "since": 1})");
    CHECK(ids(mira, "entries") == std::vector<std::string>{"character/mira", "task/voice"});
    CHECK(*mira.find("changed") == parse(R"({"rule/one_hit": null})"));
    CHECK(ids(engine.ok("storyboard.brief", R"({"entities": ["crate"]})"), "entries") ==
          std::vector<std::string>{"item/crate"});

    const auto unknown =
        engine.fails("storyboard.brief", R"({"entities": ["mria"], "ids": ["character/mia"]})",
                     command::code::kNotFound);
    const json::Value& unknown_errors = *unknown.data.find("errors");
    REQUIRE(unknown_errors.size() == 2);
    CHECK(unknown_errors.as_array()[0].find("path")->as_string() == "/ids/0");
    CHECK(contains(unknown_errors.as_array()[0].find("message")->as_string(),
                   "did you mean \"character/mira\"?"));
    CHECK(unknown_errors.as_array()[1].find("path")->as_string() == "/entities/0");
    CHECK(contains(unknown_errors.as_array()[1].find("message")->as_string(),
                   "did you mean \"mira\"?"));

    // check reads every scene and prefab, and the open scene as it is now. Hand edits to the
    // storyboard are checked too.
    engine.ok("component.set",
              R"({"id": "mira", "components": {"StoryRef": {"ids": ["character/mia"]}}})");
    write("storyboard/story.json", R"({"entries": {"beat/arrival": {"name": "Arrival",
        "place": "place/roof", "summary": "The runners land."}}, "format": 1})");
    const json::Value checked = engine.ok("storyboard.check");
    CHECK_FALSE(checked.find("valid")->as_bool());
    std::vector<std::string> errors;
    for (const json::Value& error : checked.find("errors")->as_array()) {
        errors.push_back(error.find("file")->as_string() + " " + error.find("path")->as_string());
    }
    CHECK(errors == std::vector<std::string>{
                        "storyboard/story.json /entries/beat~1arrival/place",
                        "storyboard/story.json /entries/beat~1arrival/summary",
                        "scenes/roof.scene.json /entities/sign/components/StoryRef/ids/0",
                        "scenes/docks.scene.json /entities/mira/components/StoryRef/ids/0",
                    });
    CHECK(*checked.find("warnings") == parse(R"([{"file": "storyboard/world.json",
        "message": "links to \"art/crate.png\", which does not exist",
        "path": "/entries/item~1crate/links/1"}])"));
    std::filesystem::remove_all(root);
}

TEST_CASE("a file changed on disk while a command ran is not overwritten") {
    const std::string root = "command_conflict_project";
    std::filesystem::remove_all(root);
    REQUIRE(slash::platform::create_directories(root).ok());
    REQUIRE(slash::platform::write_file(root + "/notes.txt", "first").ok());
    command::SessionOptions options;
    options.project_root = root;
    Engine engine(options);
    // A command that reads a file, then finds another agent wrote it before it staged its own.
    engine.commands.add({
        .name = "test.race",
        .description = "Stages a file that changes on disk meanwhile.",
        .capabilities = {Capability::Edit},
        .effect = command::Effect::Edit,
        .params = parse(R"({"type": "object"})"),
        .handler =
            [&](command::Session& session, const json::Value& /*params*/) {
                REQUIRE(session.read_project_file("notes.txt").value() == "first");
                REQUIRE(slash::platform::write_file(root + "/notes.txt", "theirs").ok());
                REQUIRE(session.stage_file("notes.txt", "mine").ok());
                REQUIRE(session.stage_append("log.txt", "line\n").ok());
                return command::Reply::success(json::Object{});
            },
    });
    const auto reply = engine.fails("test.race", "{}", command::code::kConflict);
    CHECK(contains(reply.message, "\"notes.txt\" changed on disk"));
    CHECK(slash::platform::read_file(root + "/notes.txt").value() == "theirs");
    CHECK_FALSE(slash::platform::exists(root + "/log.txt"));
    CHECK(engine.session->undo_labels().empty());
    // Read commands cannot stage files.
    CHECK_FALSE(engine.session->stage_file("notes.txt", "x").ok());
    std::filesystem::remove_all(root);
}

TEST_CASE("step advances the simulation and undo reverts it") {
    Engine engine;
    engine.ok("entity.create", R"({"id": "ball", "components": {"Transform2D": {}}})");
    engine.session->world().add_system("fall", [](world::World& w, double seconds) {
        w.ecs().view<world::Transform2D>().each([&](world::Transform2D& transform) {
            transform.position.y += static_cast<float>(seconds * 60.0);
        });
    });
    CHECK(engine.ok("step", R"({"ticks": 3})") == parse(R"({"ticks": 3, "tick": 3})"));
    const auto y = [&] {
        return engine.ok("entity.get", R"({"id": "ball"})")
            .find("components")
            ->find("Transform2D")
            ->find("position")
            ->as_array()[1]
            .as_double();
    };
    CHECK(y() == doctest::Approx(3.0));
    engine.ok("undo");
    CHECK(y() == doctest::Approx(0.0));
}

TEST_CASE("run.until steps until entities match, or until none do") {
    Engine engine;
    engine.ok("batch", R"({"commands": [
        {"command": "entity.create", "params": {"id": "ball", "components": {
            "Transform2D": {}, "Health": {}}}},
        {"command": "entity.create", "params": {"id": "wall", "components": {
            "Transform2D": {"position": [0, -50]}}}}]})");
    // The ball falls one pixel a tick and loses ten hit points a tick from tick 21 on.
    engine.session->world().add_system("fall", [](world::World& w, double /*seconds*/) {
        w.ecs().view<world::Transform2D, cmdtest::Health>().each(
            [&](world::Transform2D& transform, cmdtest::Health& health) {
                transform.position.y += 1.0F;
                if (transform.position.y > 20.0F) {
                    health.current -= 10.0F;
                }
            });
    });

    const std::string_view below_floor =
        R"({"where": [{"component": "Transform2D", "field": "position.y", "op": ">", "value": 10}]})";
    CHECK(engine.ok("run.until", below_floor) == parse(R"({
        "count": 1, "entities": ["ball"], "matched": true, "tick": 11, "ticks": 11,
        "truncated": false
    })"));
    // A condition that already holds runs no ticks.
    CHECK(engine.ok("run.until", below_floor).find("ticks")->as_int() == 0);

    // With include, the matching entities come with their components.
    CHECK(engine.ok("run.until", R"({"where": [{"component": "Health", "field": "current",
                                     "op": "<=", "value": 50}],
                                     "include": ["Health"]})") == parse(R"({
        "count": 1, "matched": true, "tick": 25, "ticks": 14, "truncated": false,
        "entities": [{"id": "ball", "components": {"Health": {"current": 50.0, "max": 100.0}}}]
    })"));

    // until "none" waits for the last match to go; the reply then lists no entities.
    CHECK(engine.ok("run.until", R"({"until": "none", "where": [{"component": "Health",
                                     "field": "current", "op": ">", "value": 0}]})") ==
          parse(R"({"count": 0, "entities": [], "matched": true, "tick": 30, "ticks": 5,
                    "truncated": false})"));

    // The tick limit stops a condition that never holds; the reply says it did not match.
    CHECK(engine.ok("run.until", R"({"with": ["Follow"], "ticks": 7})") == parse(R"({
        "count": 0, "entities": [], "matched": false, "tick": 37, "ticks": 7, "truncated": false
    })"));

    // A dry run reports where the world would be and leaves it; undo reverts a real run. The
    // ball's hit points are below Health's minimum of 0 by now, and the world still restores.
    const json::Value dry = engine.ok("run.until", R"({"dry_run": true, "where": [
        {"component": "Transform2D", "field": "position.y", "op": ">=", "value": 40}]})");
    CHECK(dry.find("result")->find("ticks")->as_int() == 3);
    CHECK(*dry.find("changes") == parse(R"({"changed": {"ball": {"Health": {"current": -100.0},
                                         "Transform2D": {"position": [0.0, 40.0]}}}})"));
    const json::Value before = engine.scene();
    engine.ok("run.until", R"({"with": ["Follow"], "ticks": 2})");
    CHECK(engine.scene() != before);
    engine.ok("undo");
    CHECK(engine.scene() == before);

    // It needs something to wait for, and bad conditions point at the bad value.
    const command::Reply nothing =
        engine.fails("run.until", R"({"ticks": 5})", command::code::kInvalidParams);
    CHECK(contains(nothing.message, "give with, without or where"));
    const command::Reply typo =
        engine.fails("run.until", R"({"where": [{"component": "Transform2D", "field": "positon.y",
                                    "value": 1}]})",
                     command::code::kInvalidParams);
    CHECK(typo.data.find("path")->as_string() == "/where/0/field");
    engine.fails("run.until", R"({"with": ["Helth"]})", command::code::kNotFound);
    engine.fails("run.until", R"({"with": ["Health"], "until": "all"})",
                 command::code::kInvalidParams);
}

TEST_CASE("engine.describe lists systems in the order they run") {
    Engine engine;
    world::World& world = engine.session->world();
    world.add_system("movement", [](world::World& /*w*/, double /*dt*/) {},
                     {.doc = "Moves things by their velocity."});
    world.add_system("controls", [](world::World& /*w*/, double /*dt*/) {}, {.priority = -1});
    // Every session applies input first and plays animations, steps 2D physics, moves the
    // camera and parallax, runs particles and plays sound with built-in systems that run late.
    CHECK(*engine.ok("engine.describe").find("systems") ==
          parse(R"(["input", "gui", "controls", "movement", "animation_player", "physics2d",
                    "animated_sprite_2d", "camera_2d", "parallax_2d", "cpu_particles_2d",
                    "audio"])"));
    CHECK(*engine.ok("engine.describe", R"({"full": true})").find("systems") == parse(R"([
        {"name": "input", "doc": "Applies the input events queued since the last tick.",
         "priority": -1000},
        {"name": "gui", "doc": "Sends input events to controls: hover, focus, buttons, sliders and text fields.",
         "priority": -900},
        {"name": "controls", "priority": -1},
        {"name": "movement", "doc": "Moves things by their velocity.", "priority": 0},
        {"name": "animation_player",
         "doc": "Plays each AnimationPlayer's animation on its entity's components.",
         "priority": 900},
        {"name": "physics2d",
         "doc": "Steps 2D physics: moves rigid and character bodies and reports contacts.",
         "priority": 950},
        {"name": "animated_sprite_2d",
         "doc": "Plays each AnimatedSprite2D's animation on its Sprite2D.", "priority": 1000},
        {"name": "camera_2d",
         "doc": "Moves the current Camera2D's drag margins and smoothing on by the tick.",
         "priority": 1050},
        {"name": "parallax_2d",
         "doc": "Scrolls each Parallax2D with the current camera and its autoscroll.",
         "priority": 1060},
        {"name": "cpu_particles_2d",
         "doc": "Emits, moves and ages each CPUParticles2D's particles.", "priority": 1100},
        {"name": "audio",
         "doc": "Plays each AudioStreamPlayer and AudioStreamPlayer2D and mixes the tick's sound through the bus layout.",
         "priority": 1200}
    ])"));
}

TEST_CASE("play and pause set whether a running game ticks") {
    Engine plain;
    // Sessions with no clock of their own have no play or pause; they use step.
    plain.fails("play", "{}", command::code::kMethodNotFound);
    CHECK(plain.ok("engine.describe").find("scene")->find("playing") == nullptr);

    world::ComponentRegistry components;
    world::register_builtin_components(components);
    command::Registry commands;
    command::register_builtin_commands(commands);
    command::register_run_commands(commands);
    command::SessionOptions options;
    options.capture_logs = false;
    command::Session session(components, commands, options);
    CHECK_FALSE(session.playing());
    command::Reply reply = session.call("play", json::Value());
    REQUIRE(reply.ok);
    CHECK(reply.value == parse(R"({"playing": true, "tick": 0})"));
    CHECK(session.playing());
    CHECK(session.call("engine.describe", json::Value())
              .value.find("scene")
              ->find("playing")
              ->as_bool());
    REQUIRE(session.call("step", parse(R"({"ticks": 2})")).ok);
    reply = session.call("pause", json::Value());
    CHECK(reply.value == parse(R"({"playing": false, "tick": 2})"));
    CHECK_FALSE(session.playing());
    // Neither changes the world, so there is nothing to undo.
    CHECK(session.undo_labels() == std::vector<std::string>{"step"});

    command::SessionOptions read_only;
    read_only.capture_logs = false;
    read_only.capabilities = {Capability::Read};
    command::Session observer(components, commands, read_only);
    CHECK(observer.call("play", json::Value()).code == command::code::kPermissionDenied);
}

TEST_CASE("a running game lists displays, changes V-Sync and the frame cap, and reports pacing") {
    auto platform = slash::platform::Platform::create({.headless = true});
    REQUIRE(platform.ok());
    auto loop = slash::platform::Loop::create(platform.value(),
                                              {.tick_hz = 60.0, .max_ticks = 100, .max_fps = 0});
    REQUIRE(loop.ok());
    const auto renderer = slash::render::create_null_device();
    Engine engine;
    command::register_live_commands(
        engine.commands,
        {.platform = &platform.value(), .loop = &loop.value(), .renderer = renderer.get()});

    // Headless runs have no displays.
    CHECK(engine.ok("display.list") ==
          parse(R"({"displays": [], "max_fps": 0, "vsync_mode": "enabled"})"));
    CHECK(engine.ok("display.set", R"({"vsync_mode": "mailbox", "max_fps": 120})") ==
          parse(R"({"max_fps": 120, "vsync_mode": "mailbox"})"));
    CHECK(loop.value().max_fps() == 120);
    CHECK(renderer->vsync() == slash::render::VsyncMode::Mailbox);
    CHECK(engine.ok("display.set", R"({"max_fps": 0})") ==
          parse(R"({"max_fps": 0, "vsync_mode": "mailbox"})"));
    command::Reply bad =
        engine.fails("display.set", R"({"max_fps": 1001})", command::code::kInvalidParams);
    CHECK(contains(bad.message, "/max_fps"));
    bad =
        engine.fails("display.set", R"({"vsync_mode": "adaptiv"})", command::code::kInvalidParams);
    CHECK(contains(bad.message, "/vsync_mode"));
    CHECK(loop.value().max_fps() == 0);
    // Neither changes the world, so there is nothing to undo.
    CHECK(engine.session->undo_labels().empty());

    for (int i = 0; i < 3; ++i) {
        REQUIRE(loop.value().frame(nullptr));
    }
    const json::Value stats = engine.ok("perf.stats");
    CHECK(stats.find("frames")->as_int() == 3);
    CHECK(stats.find("ticks")->as_int() == 3);
    CHECK(stats.find("catch_up_frames")->as_int() == 0);
    CHECK(stats.find("tick_hz")->as_double() == doctest::Approx(60.0));
    CHECK(stats.find("renderer")->as_string() == "null");
    CHECK(stats.find("vsync_mode")->as_string() == "mailbox");
    CHECK(stats.find("frame_ms")->find("avg") != nullptr);
    CHECK(stats.find("fps") != nullptr);

    command::SessionOptions read_only;
    read_only.capture_logs = false;
    read_only.capabilities = {Capability::Read};
    command::Session observer(engine.components, engine.commands, read_only);
    CHECK(observer.call("display.list", json::Value()).ok);
    CHECK(observer.call("display.set", parse(R"({"max_fps": 60})")).code ==
          command::code::kPermissionDenied);
}

TEST_CASE("schema.get returns scene, component and command schemas") {
    Engine engine;
    CHECK(engine.ok("schema.get", R"({"kind": "prefab"})").find("title")->as_string() ==
          "SlashEngine prefab");
    CHECK(engine.ok("schema.get", R"({"kind": "scene"})").find("title")->as_string() ==
          "SlashEngine scene");
    CHECK(engine.ok("schema.get", R"({"kind": "storyboard"})").find("title")->as_string() ==
          "SlashEngine storyboard file");
    CHECK(engine.ok("schema.get", R"({"kind": "frames"})").find("title")->as_string() ==
          "SlashEngine sprite frames");
    CHECK(engine.ok("schema.get", R"({"kind": "material"})").find("title")->as_string() ==
          "SlashEngine material");
    CHECK(engine.ok("schema.get", R"({"kind": "animation"})").find("title")->as_string() ==
          "SlashEngine animation library");
    CHECK(engine.ok("schema.get", R"({"kind": "theme"})").find("title")->as_string() ==
          "SlashEngine theme");

    const json::Value health =
        engine.ok("schema.get", R"({"kind": "component", "name": "Health"})");
    CHECK(json::validate(parse(R"({"current": 5})"), health).empty());
    CHECK_FALSE(json::validate(parse(R"({"current": -5})"), health).empty());

    const json::Value step = engine.ok("schema.get", R"({"kind": "command", "name": "step"})");
    CHECK(step.find("properties")->find("ticks") != nullptr);

    engine.fails("schema.get", R"({"kind": "component"})", command::code::kInvalidParams);
    const auto reply = engine.fails("schema.get", R"({"kind": "command", "name": "stpe"})",
                                    command::code::kNotFound);
    CHECK(contains(reply.message, "did you mean \"step\""));
}

TEST_CASE("material.eval returns the colors a material draws, for a file or a sprite") {
    const std::string root = "material_test_project";
    std::filesystem::remove_all(root);
    REQUIRE(slash::platform::create_directories(root).ok());
    render::Image red;
    red.width = 1;
    red.height = 1;
    red.pixels = {255, 0, 0, 255};
    REQUIRE(slash::platform::write_file(root + "/red.png", render::encode_png(red).value()).ok());
    REQUIRE(slash::platform::write_file(root + "/flash.material.json", R"({
      "color": {"op": "vec4", "args": [
        {"op": "mix", "args": ["base.rgb", "flash_color.rgb", "flash"]}, "base.a"]},
      "format": 1,
      "nodes": {
        "base": {"op": "mul", "args": [{"op": "texture", "args": ["texture", "uv"]}, "color"]}
      },
      "params": {
        "flash": {"type": "float", "value": 0, "min": 0, "max": 1},
        "flash_color": {"type": "color", "value": [1, 1, 1, 1]}
      }
    })")
                .ok());
    command::SessionOptions options;
    options.project_root = root;
    Engine engine(options);
    engine.ok("entity.create", R"({"id": "hero", "components": {"Transform2D": {},
        "Sprite2D": {"texture": "red.png", "material": "flash.material.json",
                     "material_params": {"flash": 0.5}, "modulate": [1, 1, 1, 0.5]}}})");
    engine.ok("entity.create", R"({"id": "plain", "components": {"Transform2D": {},
        "Sprite2D": {"texture": "red.png"}}})");
    engine.ok("entity.create", R"({"id": "empty", "components": {"Transform2D": {}}})");

    // A sprite: its texture, params and modulate.
    CHECK(engine.ok("material.eval", R"({"id": "hero", "uv": [[0.5, 0.5]]})") ==
          parse(R"({"colors": [[1, 0.5, 0.5, 0.5]]})"));
    // Params given here win over the sprite's.
    CHECK(engine.ok("material.eval", R"({"id": "hero", "uv": [[0.5, 0.5]],
                                         "params": {"flash": 1}, "color": [1, 1, 1, 1]})") ==
          parse(R"({"colors": [[1, 1, 1, 1]]})"));
    // A sprite with no material draws its texture times its modulate.
    CHECK(engine.ok("material.eval", R"({"id": "plain", "uv": [[0, 0], [1, 1]]})") ==
          parse(R"({"colors": [[1, 0, 0, 1], [1, 0, 0, 1]]})"));
    // A file on its own samples white, unless given a texture.
    CHECK(engine.ok("material.eval", R"({"path": "flash.material.json", "uv": [[0.5, 0.5]],
                                         "params": {"flash": 0.25,
                                                    "flash_color": [0, 0, 0]}})") ==
          parse(R"({"colors": [[0.75, 0.75, 0.75, 1]]})"));
    CHECK(engine.ok("material.eval", R"({"path": "flash.material.json", "uv": [[0.5, 0.5]],
                                         "texture": "red.png"})") ==
          parse(R"({"colors": [[1, 0, 0, 1]]})"));

    auto reply =
        engine.fails("material.eval", R"({"uv": [[0, 0]]})", command::code::kInvalidParams);
    CHECK(contains(reply.message, "give id"));
    engine.fails("material.eval", R"({"id": "hero", "path": "flash.material.json",
                                      "uv": [[0, 0]]})",
                 command::code::kInvalidParams);
    reply = engine.fails("material.eval", R"({"id": "hero", "uv": [[0, 0]],
                                              "params": {"flsh": 1}})",
                         command::code::kInvalidArgument);
    CHECK(reply.message ==
          "/params/flsh: the material has no param \"flsh\"; did you mean \"flash\"?");
    CHECK(reply.data.find("path")->as_string() == "/params/flsh");
    engine.ok("component.set", R"({"id": "plain", "components": {"Sprite2D": {
        "material": "flash.material.json", "material_params": {"flash": 3}}}})");
    reply = engine.fails("material.eval", R"({"id": "plain", "uv": [[0, 0]]})",
                         command::code::kInvalidArgument);
    CHECK(reply.message ==
          "/id: material_params/flash: 3 is outside the param's range, from 0 to 1");
    reply = engine.fails("material.eval", R"({"id": "empty", "uv": [[0, 0]]})",
                         command::code::kInvalidParams);
    CHECK(reply.message == "/id: \"empty\" has no Sprite2D");
    reply = engine.fails("material.eval", R"({"id": "hreo", "uv": [[0, 0]]})",
                         command::code::kNotFound);
    CHECK(contains(reply.message, "did you mean \"hero\""));
    reply = engine.fails("material.eval", R"({"path": "gone.material.json", "uv": [[0, 0]]})",
                         command::code::kNotFound);
    CHECK(reply.data.find("path")->as_string() == "/path");
    reply = engine.fails("material.eval", R"({"path": "flash.material.json", "uv": [[0, 0]],
                                              "texture": "gone.png"})",
                         command::code::kNotFound);
    CHECK(contains(reply.message, "gone.png"));
    engine.fails("material.eval", R"({"path": "flash.material.json", "uv": []})",
                 command::code::kInvalidParams);
    std::filesystem::remove_all(root);
}

TEST_CASE("physics commands find bodies by ray, point and shape, and what touches them") {
    Engine engine;
    engine.ok("batch", R"({"commands": [
        {"command": "entity.create", "params": {"id": "floor", "components": {
            "Transform2D": {"position": [160, 170]}, "StaticBody2D": {},
            "CollisionShape2D": {"size": [320, 20]}}}},
        {"command": "entity.create", "params": {"id": "crate", "components": {
            "Transform2D": {"position": [100, 40]}, "RigidBody2D": {"contact_monitor": true},
            "CollisionShape2D": {"size": [24, 24]}}}},
        {"command": "entity.create", "params": {"id": "coin", "components": {
            "Transform2D": {"position": [100, 100]}, "Area2D": {},
            "CollisionShape2D": {"shape": "circle", "radius": 8}}}}]})");
    engine.ok("events.subscribe", R"({"kinds": ["body_entered", "body_exited"]})");

    // Before any step, queries see the bodies where the components put them.
    CHECK(engine.ok("physics.raycast", R"({"from": [0, 40], "to": [200, 40]})") ==
          parse(R"({"hit": {"id": "crate", "point": [88, 40], "normal": [-1, 0],
                            "fraction": 0.44}})"));
    CHECK(engine.ok("physics.raycast", R"({"from": [100, 0], "to": [100, 300],
                                           "collide_with_areas": true, "exclude": ["crate"]})") ==
          parse(R"({"hit": {"id": "coin", "point": [100, 92], "normal": [0, -1],
                            "fraction": 0.3067}})"));
    CHECK(engine.ok("physics.raycast", R"({"from": [0, 0], "to": [10, 0]})") ==
          parse(R"({"hit": null})"));

    engine.ok("step", R"({"ticks": 60})");
    const json::Value crate = engine.ok("entity.get", R"({"id": "crate"})");
    CHECK(crate.find("components")
              ->find("Transform2D")
              ->find("position")
              ->as_array()[1]
              .as_double() == doctest::Approx(148.0).epsilon(0.001));
    CHECK(engine.ok("physics.contacts", R"({"id": "crate"})") ==
          parse(R"({"bodies": ["floor"], "areas": []})"));
    CHECK(engine.ok("physics.overlap", R"({"point": [100, 150]})") ==
          parse(R"({"ids": ["crate"]})"));
    CHECK(engine.ok("physics.overlap", R"({"point": [100, 100], "collide_with_areas": true,
                                           "collide_with_bodies": false})") ==
          parse(R"({"ids": ["coin"]})"));
    CHECK(
        engine.ok("physics.overlap", R"({"shape": {"size": [300, 40]}, "position": [150, 160]})") ==
        parse(R"({"ids": ["crate", "floor"]})"));
    CHECK(engine.ok("physics.overlap", R"({"shape": {"size": [300, 40]}, "position": [150, 160],
                                           "max_results": 1, "collision_mask": 1})") ==
          parse(R"({"ids": ["crate"]})"));
    // It fell through the coin's area on the way down.
    CHECK(*engine.ok("events.read", R"({"subscription": "events_1"})").find("events") == parse(R"([
        {"seq": 1, "tick": 17, "kind": "body_entered", "id": "coin", "data": {"body": "crate"}},
        {"seq": 2, "tick": 24, "kind": "body_exited", "id": "coin", "data": {"body": "crate"}},
        {"seq": 3, "tick": 30, "kind": "body_entered", "id": "crate", "data": {"body": "floor"}}
    ])"));

    // Undo puts the crate back in the air without a stray body_exited, and the same steps land
    // it in the same place.
    engine.ok("undo");
    CHECK(engine.ok("physics.contacts", R"({"id": "crate"})") ==
          parse(R"({"bodies": [], "areas": []})"));
    engine.ok("step");
    CHECK(engine.ok("events.read", R"({"subscription": "events_1"})").find("events")->size() == 0);
    engine.ok("step", R"({"ticks": 59})");
    CHECK(engine.ok("entity.get", R"({"id": "crate"})") == crate);

    engine.fails("physics.overlap", R"({})", command::code::kInvalidParams);
    engine.fails("physics.overlap", R"({"point": [0, 0], "shape": {}})",
                 command::code::kInvalidParams);
    CHECK(engine
              .fails("physics.overlap", R"({"shape": {"shape": "circel"}})",
                     command::code::kInvalidParams)
              .message == "/shape/shape: unknown shape \"circel\"; did you mean \"circle\"?");
    CHECK(engine
              .fails("physics.overlap", R"({"shape": {"radius": "big"}})",
                     command::code::kInvalidParams)
              .data.find("path")
              ->as_string() == "/shape/radius");
    CHECK(engine
              .fails("physics.raycast", R"({"from": [0, 0], "to": [1, 0], "exclude": ["crat"]})",
                     command::code::kNotFound)
              .message == "/exclude/0: no entity has the id \"crat\"; did you mean \"crate\"?");
    engine.fails("physics.contacts", R"({"id": "nobody"})", command::code::kNotFound);
}

TEST_CASE("particles.eval counts and lists particles, as they are or after a time") {
    Engine engine;
    engine.ok("entity.create", R"({"id": "sparkler", "components": {
        "Transform2D": {"position": [100, 50]},
        "CPUParticles2D": {"amount": 10, "spread": 0, "gravity": [0, 0],
                           "initial_velocity_min": 100, "initial_velocity_max": 100}}})");
    CHECK(engine.ok("particles.eval", R"({"id": "sparkler"})") ==
          parse(R"({"alive": 0, "bounds": null})"));
    engine.ok("step", R"({"ticks": 45})");
    CHECK(engine.ok("particles.eval", R"({"id": "sparkler", "list": true, "max_results": 2})") ==
          parse(R"({"alive": 8, "bounds": [105, 50, 70.0001, 0], "particles": [
              {"age": 0.75, "color": [1, 1, 1, 1], "position": [175.0001, 50], "rotation": 0,
               "scale": 1},
              {"age": 0.65, "color": [1, 1, 1, 1], "position": [165.0001, 50], "rotation": 0,
               "scale": 1}]})"));
    // A time runs the settings from the start, apart from the world, as stepping would.
    CHECK(engine.ok("particles.eval", R"({"id": "sparkler", "time": 0.75})") ==
          parse(R"({"alive": 8, "bounds": [105, 50, 70.0001, 0]})"));
    // A failed command and a dry run put back a copy of the world; the particles stay.
    engine.fails("component.set",
                 R"({"id": "sparkler", "components": {"CPUParticles2D": {"amount": 0}}})",
                 command::code::kInvalidParams);
    engine.ok("entity.destroy", R"({"id": "sparkler", "dry_run": true})");
    CHECK(engine.ok("particles.eval", R"({"id": "sparkler"})") ==
          parse(R"({"alive": 8, "bounds": [105, 50, 70.0001, 0]})"));

    // Particles in local coordinates are placed in the world by the entity's transform.
    engine.ok("component.set", R"({"id": "sparkler", "components": {
        "Transform2D": {"rotation": 1.5707964}, "CPUParticles2D": {"local_coords": true}}})");
    CHECK(engine.ok("particles.eval", R"({"id": "sparkler", "time": 0.25, "list": true,
                                          "max_results": 1})") ==
          parse(R"({"alive": 3, "bounds": [100, 55, 0, 20], "particles": [
              {"age": 0.25, "color": [1, 1, 1, 1], "position": [100, 75], "rotation": 1.5708,
               "scale": 1}]})"));

    CHECK(
        engine.fails("particles.eval", R"({"id": "sparkle"})", command::code::kNotFound).message ==
        "/id: no entity has the id \"sparkle\"; did you mean \"sparkler\"?");
    engine.ok("entity.create", R"({"id": "rock", "components": {"Transform2D": {}}})");
    CHECK(engine.fails("particles.eval", R"({"id": "rock"})", command::code::kInvalidParams)
              .message == "/id: \"rock\" has no CPUParticles2D");
    engine.fails("particles.eval", R"({"id": "sparkler", "time": -1})",
                 command::code::kInvalidParams);
}

TEST_CASE("ui.layout returns where controls are and the lines labels show") {
    const std::string root = "ui_test_project";
    std::filesystem::remove_all(root);
    REQUIRE(slash::platform::create_directories(root).ok());
    REQUIRE(slash::platform::write_file(root + "/project.json",
                                        R"({"format": 1, "name": "UI", "version": "0.1.0",
                                            "main_scene": "main.scene.json",
                                            "window": {"width": 400, "height": 300},
                                            "gui": {"allow_system_fallback": false}})")
                .ok());
    command::SessionOptions options;
    options.project_root = root;
    Engine engine(options);
    engine.ok("batch", R"({"commands": [
        {"command": "entity.create", "params": {"id": "title", "components": {
            "Control": {"anchor_right": 1, "offset_top": 10},
            "Label": {"text": "Paused", "horizontal_alignment": "center"}}}},
        {"command": "entity.create", "params": {"id": "hint", "components": {
            "Control": {"offset_left": 20, "offset_top": 60, "offset_right": 80,
                        "visible": false},
            "Label": {"text": "Press any key", "autowrap_mode": "word"}}}},
        {"command": "entity.create", "params": {"id": "panel", "components": {
            "Control": {"offset_right": 100, "offset_bottom": 50, "children": ["title"],
                        "rotation": 1.5707964},
            "ColorRect": {}}}}]})");
    // The project's window is the viewport; the panel turns a quarter, so its bounds do too.
    CHECK(engine.ok("ui.layout") == parse(R"({"viewport": [400, 300], "controls": [
        {"id": "hint", "rect": [20, 60, 60, 47], "visible": false,
         "lines": ["Press", "any key"]},
        {"id": "panel", "rect": [-50, 0, 50, 100]},
        {"id": "title", "rect": [-32, 0, 22, 100], "lines": ["Paused"]}]})"));
    CHECK(engine.ok("ui.layout", R"({"ids": ["hint"], "viewport": [800, 600]})") ==
          parse(R"({"viewport": [800, 600], "controls": [
              {"id": "hint", "rect": [20, 60, 60, 47], "visible": false,
               "lines": ["Press", "any key"]}]})"));

    // Text that doesn't fit, a missing glyph and a font that doesn't load are reported.
    engine.ok("component.set", R"({"id": "hint", "components": {
        "Control": {"visible": true, "theme_override_fonts": {"font": "missing.ttf"}},
        "Label": {"text": "Press any key \u4e00", "clip_text": true, "autowrap_mode": "off"}}})");
    json::Value hint = engine.ok("ui.layout", R"({"ids": ["hint"]})");
    CHECK(hint["controls"].as_array().at(0) == parse(R"({"id": "hint", "rect": [20, 60, 60, 22],
        "lines": ["Press any key \u4e00"], "overflow": true, "missing_glyphs": 1,
        "font_error": "asset \"missing.ttf\" could not be read: no such file"})"));

    // A child listed twice is a problem, with its JSON Pointer.
    engine.ok("component.set", R"({"id": "hint", "components": {"Control": {
        "children": ["title"]}}})");
    CHECK(engine.ok("ui.layout", R"({"ids": ["panel"]})")["problems"] == parse(R"([
        {"id": "panel", "path": "/components/Control/children/0",
         "message": "\"title\" is already a child of \"hint\""}])"));
    // So is a theme override its type doesn't have.
    engine.ok("component.set", R"({"id": "title", "components": {"Control": {
        "theme_override_colors": {"font_colour": [1, 0, 0, 1]}}}})");
    CHECK(engine.ok("ui.layout", R"({"ids": ["title"]})")["problems"] == parse(R"([
        {"id": "title", "path": "/components/Control/theme_override_colors/font_colour",
         "message": "Label has no colors item \"font_colour\"; did you mean \"font_color\"?"}])"));

    CHECK(engine.fails("ui.layout", R"({"ids": ["titel"]})", command::code::kNotFound).message ==
          "/ids/0: no entity has the id \"titel\"; did you mean \"title\"?");
    engine.ok("entity.create", R"({"id": "rock", "components": {"Transform2D": {}}})");
    CHECK(
        engine.fails("ui.layout", R"({"ids": ["rock"]})", command::code::kInvalidParams).message ==
        "/ids/0: \"rock\" isn't a control: it has no Control or UI component");
    std::filesystem::remove_all(root);
}

TEST_CASE("a session applies project.json's settings again when the file changes") {
    const std::string root = "command_project_settings";
    std::filesystem::remove_all(root);
    REQUIRE(slash::platform::create_directories(root).ok());
    command::SessionOptions options;
    options.project_root = root;
    Engine engine(options);
    engine.ok("entity.create", R"({"id": "box", "components": {
        "Transform2D": {}, "RigidBody2D": {}, "CollisionShape2D": {}}})");
    engine.fails("input.actions", R"({"names": ["jump"]})", command::code::kInvalidParams);

    // A project.json written after the session started, as project.import writes one, takes
    // effect at the next command: its window is the viewport, its actions bind and its gravity
    // pulls.
    REQUIRE(slash::platform::write_file(root + "/project.json",
                                        R"({"format": 1, "name": "Late", "version": "0.1.0",
                                            "main_scene": "main.scene.json",
                                            "window": {"width": 400, "height": 300},
                                            "physics_2d": {"gravity": 100, "linear_damp": 0},
                                            "input": {"jump": {"events": [
                                                {"type": "key", "key": "Space"}]}}})")
                .ok());
    CHECK(engine.ok("ui.layout")["viewport"] == parse("[400, 300]"));
    CHECK(engine.ok("input.actions", R"({"names": ["jump"]})")["actions"]["jump"]["events"] ==
          parse(R"([{"key": "Space", "type": "key"}])"));
    engine.ok("step", R"({"ticks": 60})");
    const json::Value box =
        engine.ok("entity.get", R"({"id": "box", "components": ["RigidBody2D"]})");
    CHECK(box.find("components")
              ->find("RigidBody2D")
              ->find("linear_velocity")
              ->as_array()[1]
              .as_double() == doctest::Approx(100.0).epsilon(0.01));
    std::filesystem::remove_all(root);
}

TEST_CASE("ui.theme returns the theme items a control uses and where each comes from") {
    const std::string root = "ui_theme_project";
    std::filesystem::remove_all(root);
    REQUIRE(slash::platform::create_directories(root + "/ui").ok());
    REQUIRE(slash::platform::write_file(root + "/project.json",
                                        R"({"format": 1, "name": "UI", "version": "0.1.0",
                                            "main_scene": "main.scene.json",
                                            "gui": {"theme": "ui/game.theme.json"}})")
                .ok());
    REQUIRE(slash::platform::write_file(
                root + "/ui/game.theme.json",
                R"({"format": 1, "types": {"BoxContainer": {"constants": {"separation": 8}}}})")
                .ok());
    command::SessionOptions options;
    options.project_root = root;
    Engine engine(options);
    engine.ok("batch", R"({"commands": [
        {"command": "entity.create", "params": {"id": "box", "components": {
            "BoxContainer": {"vertical": true}}}},
        {"command": "entity.create", "params": {"id": "panel", "components": {
            "Control": {"theme_override_styles": {"panel": {"corner_radius": 4}}},
            "Panel": {}}}},
        {"command": "entity.create", "params": {"id": "label", "components": {
            "Label": {"text": "Hi"}}}}]})");
    CHECK(engine.ok("ui.theme", R"({"id": "box"})") == parse(R"({"items": [
        {"item": "constants/separation", "value": 8, "from": "ui/game.theme.json"}]})"));
    CHECK(engine.ok("ui.theme", R"({"id": "panel"})") == parse(R"({"items": [
        {"item": "styles/panel", "value": {"corner_radius": 4}, "from": "override"}]})"));
    CHECK(
        engine.ok("ui.theme", R"({"id": "label", "items": ["font_sizes", "colors/font_color"]})") ==
        parse(R"({"items": [
              {"item": "colors/font_color", "value": [1.0, 1.0, 1.0, 1.0], "from": "default"},
              {"item": "font_sizes/font_size", "value": 16, "from": "default"}]})"));
    CHECK(engine
              .fails("ui.theme", R"({"id": "box", "items": ["constants/separaton"]})",
                     command::code::kInvalidParams)
              .message == "/items/0: \"box\" uses no theme item \"constants/separaton\"; did you "
                          "mean \"constants/separation\"?");
    engine.ok("entity.create", R"({"id": "rock", "components": {"Transform2D": {}}})");
    CHECK(engine.fails("ui.theme", R"({"id": "rock"})", command::code::kInvalidParams).message ==
          "/id: \"rock\" isn't a control: it has no Control or UI component");
    std::filesystem::remove_all(root);
}

TEST_CASE("text.measure returns text's size and lines in a font") {
    Engine engine;
    CHECK(engine.ok("text.measure", R"({"text": "Hello\nHello, world"})") ==
          parse(R"({"size": [82, 44], "line_height": 22, "ascent": 17,
                    "lines": ["Hello", "Hello, world"]})"));
    CHECK(engine.ok("text.measure", R"({"text": "Hello world again", "autowrap_mode": "word",
                                        "width": 60, "font_size": 16})")["lines"] ==
          parse(R"(["Hello", "world", "again"])"));
    {
        // Characters no font has are counted, unless the machine has a font for them and the
        // project lets text use it.
        const std::string root = "measure_test_project";
        std::filesystem::remove_all(root);
        REQUIRE(slash::platform::create_directories(root).ok());
        REQUIRE(slash::platform::write_file(root + "/project.json",
                                            R"({"format": 1, "name": "M", "version": "0.1.0",
                                                "main_scene": "main.scene.json",
                                                "gui": {"allow_system_fallback": false}})")
                    .ok());
        command::SessionOptions options;
        options.project_root = root;
        Engine strict(options);
        CHECK(strict.ok("text.measure", R"({"text": "\u4e00"})")["missing_glyphs"] == parse("1"));
        const json::Value any = engine.ok("text.measure", R"({"text": "\u4e00"})");
        const bool counted =
            any.find("missing_glyphs") != nullptr || any.find("system_glyphs") != nullptr;
        CHECK(counted);
        // Lines that go right to left are named: a Hebrew paragraph, or any text made to.
        json::Value hebrew =
            strict.ok("text.measure", R"({"text": "abc\n\u05e9\u05dc\u05d5\u05dd"})");
        CHECK(hebrew["rtl"] == parse("[false, true]"));
        CHECK(hebrew["missing_glyphs"] == parse("4"));
        CHECK(strict.ok("text.measure", R"({"text": "abc", "direction": "rtl"})")["rtl"] ==
              parse("true"));
        CHECK(strict.ok("text.measure", R"({"text": "abc"})").find("rtl") == nullptr);
        CHECK(strict
                  .fails("text.measure", R"({"text": "abc", "language": "arabic"})",
                         command::code::kInvalidParams)
                  .message ==
              "/language: \"arabic\" isn't a BCP 47 language tag, such as \"ar\" or \"zh-Hant\"");
        std::filesystem::remove_all(root);
    }
    CHECK(engine
              .fails("text.measure", R"({"text": "x", "autowrap_mode": "word"})",
                     command::code::kInvalidParams)
              .message == "/width: autowrap_mode needs a width to wrap to");
    CHECK(engine
              .fails("text.measure", R"({"text": "x", "font": "missing.otf"})",
                     command::code::kInvalidParams)
              .message == "/font: asset \"missing.otf\" could not be read: no such file");
    engine.fails("text.measure", R"({"text": "x", "font_size": 0})", command::code::kInvalidParams);
}

TEST_CASE("light.sample returns how lit points are, and which lights reach them") {
    const std::string root = "light_test_project";
    std::filesystem::remove_all(root);
    REQUIRE(slash::platform::create_directories(root).ok());
    render::Image white;
    white.width = 4;
    white.height = 4;
    white.pixels.assign(std::size_t{4} * 4 * 4, 255);
    REQUIRE(
        slash::platform::write_file(root + "/white.png", render::encode_png(white).value()).ok());
    command::SessionOptions options;
    options.project_root = root;
    Engine engine(options);

    // No lights and no CanvasModulate: draws are unshaded.
    CHECK(engine.ok("light.sample", R"({"points": [[0, 0]]})") ==
          parse(R"({"lit": false, "ambient": [1, 1, 1],
                    "points": [{"color": [1, 1, 1], "lights": []}]})"));

    // A 200-pixel square lamp with shadows, and a wall to its right.
    engine.ok("batch", R"({"commands": [
        {"command": "entity.create", "params": {"id": "dark", "components": {
            "CanvasModulate": {"color": [0.2, 0.2, 0.2, 1]}}}},
        {"command": "entity.create", "params": {"id": "lamp", "components": {
            "Transform2D": {},
            "PointLight2D": {"texture": "white.png", "texture_scale": 50,
                             "color": [1, 0.5, 0, 1], "energy": 2, "shadow_enabled": true}}}},
        {"command": "entity.create", "params": {"id": "wall", "components": {
            "Transform2D": {"position": [50, 0]},
            "LightOccluder2D": {"polygon": [[0, -10], [10, -10], [10, 10], [0, 10]]}}}},
        {"command": "entity.create", "params": {"id": "hero", "components": {
            "Transform2D": {"position": [0, 30]}, "Sprite2D": {"light_mask": 2}}}},
        {"command": "entity.create", "params": {"id": "empty"}}]})");
    CHECK(engine.ok("light.sample", R"({"points": [[0, 30], [80, 0], [150, 0]]})") ==
          parse(R"({"lit": true, "ambient": [0.2, 0.2, 0.2], "points": [
              {"color": [2.2, 1.2, 0.2],
               "lights": [{"id": "lamp", "color": [2, 1, 0], "shadow": 0}]},
              {"color": [0.2, 0.2, 0.2],
               "lights": [{"id": "lamp", "color": [0, 0, 0], "shadow": 1}]},
              {"color": [0.2, 0.2, 0.2], "lights": []}]})"));
    // The lamp's range_item_cull_mask is 1, so draws on light mask 2 stay dark.
    const json::Value dark = parse(R"({"lit": true, "ambient": [0.2, 0.2, 0.2],
                                       "points": [{"color": [0.2, 0.2, 0.2], "lights": []}]})");
    CHECK(engine.ok("light.sample", R"({"points": [[0, 30]], "light_mask": 2})") == dark);
    // An entity: its position and its sprite's light mask, unless given.
    CHECK(engine.ok("light.sample", R"({"id": "hero"})") == dark);
    CHECK(engine.ok("light.sample", R"({"id": "hero", "light_mask": 1})")
              .find("points")
              ->as_array()[0]
              .find("color")
              ->as_array()[0]
              .as_double() == 2.2);
    // z ranges: the lamp lights z indexes from -1024 to 1024.
    CHECK(engine.ok("light.sample", R"({"points": [[0, 30]], "z_index": 2000})") == dark);

    // A light that can't shine is left out with a warning.
    engine.ok("entity.create", R"({"id": "broken", "components": {"Transform2D": {},
        "PointLight2D": {"texture": "gone.png"}}})");
    auto result = engine.ok("light.sample", R"({"points": [[150, 0]]})");
    REQUIRE(result.find("warnings") != nullptr);
    CHECK(result.find("warnings")->as_array()[0].find("id")->as_string() == "broken");
    CHECK(
        contains(result.find("warnings")->as_array()[0].find("message")->as_string(), "gone.png"));

    auto reply = engine.fails("light.sample", "{}", command::code::kInvalidParams);
    CHECK(contains(reply.message, "give points"));
    engine.fails("light.sample", R"({"id": "hero", "points": [[0, 0]]})",
                 command::code::kInvalidParams);
    reply = engine.fails("light.sample", R"({"id": "hreo"})", command::code::kNotFound);
    CHECK(contains(reply.message, "did you mean \"hero\""));
    reply = engine.fails("light.sample", R"({"id": "empty"})", command::code::kInvalidParams);
    CHECK(reply.message == "/id: \"empty\" has no Transform2D");
    engine.fails("light.sample", R"({"points": []})", command::code::kInvalidParams);
    engine.fails("light.sample", R"({"points": [[0, 0]], "light_mask": -1})",
                 command::code::kInvalidParams);
    std::filesystem::remove_all(root);
}

TEST_CASE("frame.capture draws the window on the CPU and returns pixels, a PNG or a file") {
    const std::string root = "command_capture_project";
    std::filesystem::remove_all(root);
    REQUIRE(slash::platform::create_directories(root).ok());
    REQUIRE(slash::platform::write_file(root + "/project.json", R"({"format": 1, "name": "Capture",
        "main_scene": "main.scene.json", "version": "1.0.0", "window": {"width": 160, "height": 90},
        "rendering": {"clear_color": [0, 0, 0.5, 1]}})")
                .ok());
    render::Image white;
    white.width = 4;
    white.height = 4;
    white.pixels.assign(std::size_t{4} * 4 * 4, 255);
    REQUIRE(
        slash::platform::write_file(root + "/white.png", render::encode_png(white).value()).ok());
    command::SessionOptions options;
    options.project_root = root;
    Engine engine(options);

    // An empty world is the project's clear color.
    CHECK(engine.ok("frame.capture", R"({"points": [[0, 0]]})") ==
          parse(R"({"width": 160, "height": 90, "drawn": {}, "colors": [[0, 0, 0.502, 1]]})"));

    // A red 40-pixel square around (40, 30), and a label's background.
    engine.ok("batch", R"({"commands": [
        {"command": "entity.create", "params": {"id": "box", "components": {
            "Transform2D": {"position": [40, 30], "scale": [10, 10]},
            "Sprite2D": {"texture": "white.png", "modulate": [1, 0, 0, 1]}}}},
        {"command": "entity.create", "params": {"id": "bar", "components": {
            "Control": {"offset_left": 100, "offset_top": 60, "offset_right": 160,
                        "offset_bottom": 90},
            "ColorRect": {"color": [0, 1, 0, 0.5]}}}}]})");
    CHECK(engine.ok("frame.capture", R"({"points": [[40, 30], [20, 10], [59.5, 49.5], [60, 50],
                                                     [130, 80]]})") ==
          parse(R"({"width": 160, "height": 90, "drawn": {"sprites": 1, "controls": 1},
                    "colors": [[1, 0, 0, 1], [1, 0, 0, 1], [1, 0, 0, 1], [0, 0, 0.502, 1],
                               [0, 0.502, 0.251, 1]]})"));

    // A region crops, and scale sizes the image; points stay window pixels.
    CHECK(engine.ok("frame.capture", R"({"region": [20, 10, 50, 40], "scale": 0.5,
                                         "points": [[65, 45]]})") ==
          parse(R"({"width": 25, "height": 20, "drawn": {"sprites": 1, "controls": 1},
                    "colors": [[0, 0, 0.502, 1]]})"));

    // "image" returns the PNG as base64; MCP clients get it as an image.
    const json::Value image =
        engine.ok("frame.capture", R"({"image": true, "region": [20, 10, 40, 40]})");
    CHECK(image.find("png")->as_string().starts_with("iVBORw0KGgo"));

    // "path" writes the PNG.
    CHECK(
        *engine.ok("frame.capture", R"({"path": "shots/frame.png", "scale": 0.25})").find("path") ==
        parse(R"("shots/frame.png")"));
    const auto png = slash::platform::read_file(root + "/shots/frame.png");
    REQUIRE(png.ok());
    const auto decoded = render::decode_image(png.value());
    REQUIRE(decoded.ok());
    CHECK(decoded.value().width == 40);
    CHECK(decoded.value().height == 23);

    const auto outside = engine.fails("frame.capture", R"({"points": [[0, 0], [160, 0]]})",
                                      command::code::kInvalidArgument);
    CHECK(outside.message == "/points/1: outside the 160x90 window");
    engine.fails("frame.capture", R"({"region": [100, 50, 80, 10]})",
                 command::code::kInvalidArgument);
    engine.fails("frame.capture", R"({"path": "frame.jpg"})", command::code::kInvalidArgument);
    engine.fails("frame.capture", R"({"scale": 8})", command::code::kInvalidParams);

    // Writing a capture needs the filesystem capability.
    options.capabilities = {Capability::Read};
    Engine reader(options);
    reader.fails("frame.capture", R"({"path": "x.png"})", command::code::kPermissionDenied);
    CHECK(reader.ok("frame.capture").find("width")->as_int() == 160);
}

TEST_CASE("animation.eval returns the values an animation's tracks set, and step plays them") {
    const std::string root = "animation_test_project";
    std::filesystem::remove_all(root);
    REQUIRE(slash::platform::create_directories(root).ok());
    REQUIRE(slash::platform::write_file(root + "/hero.anim.json", R"({
      "animations": {
        "bob": {"length": 1, "loop": "linear", "tracks": [
          {"path": "Transform2D/position/y", "interpolation": "cubic",
           "keys": [{"time": 0, "value": 0}, {"time": 0.5, "value": -8}]},
          {"path": "Sprite2D/frame", "keys": [{"time": 0, "value": 0}, {"time": 0.5, "value": 1}]},
          {"event": "footstep", "keys": [{"time": 0.5, "data": {"foot": "left"}}]}]},
        "fade": {"length": 0.5, "tracks": [
          {"path": "Sprite2D/modulate/a", "keys": [{"time": 0, "value": 1}, {"time": 0.5, "value": 0}]},
          {"target": "shadow", "path": "Sprite2D/modulate/a",
           "keys": [{"time": 0, "value": 0.5}, {"time": 0.5, "value": 0}]},
          {"path": "Health/current", "keys": [{"time": 0, "value": 10}]}]}
      },
      "format": 1
    })")
                .ok());
    command::SessionOptions options;
    options.project_root = root;
    Engine engine(options);
    engine.ok("entity.create", R"({"id": "hero", "components": {"Transform2D": {},
        "AnimationPlayer": {"library": "hero.anim.json", "animation": "bob"}}})");
    engine.ok("entity.create", R"({"id": "empty", "components": {"Transform2D": {}}})");

    // An entity's player: its library and animation. Values are rounded to four decimals.
    CHECK(engine.ok("animation.eval", R"({"id": "hero", "times": [0, 0.125, 0.25, 0.5]})") ==
          parse(R"({"length": 1, "loop": "linear", "tracks": {
                      "Sprite2D/frame": [0, 0, 0, 1],
                      "Transform2D/position/y": [0, -1.25, -4, -8]}})"));
    // Another animation from its library, and only some tracks.
    CHECK(engine.ok("animation.eval", R"({"id": "hero", "animation": "fade", "times": [0.125],
                                          "tracks": ["Sprite2D/modulate/a"]})") ==
          parse(R"({"length": 0.5, "loop": "none", "tracks": {"Sprite2D/modulate/a": [0.75]}})"));
    // A track with a target is named after it, as Godot's "Node:property".
    CHECK(engine.ok("animation.eval", R"({"path": "hero.anim.json", "animation": "fade",
                                          "times": [0.5]})") ==
          parse(R"({"length": 0.5, "loop": "none",
                    "tracks": {"Health/current": [10], "Sprite2D/modulate/a": [0],
                               "shadow:Sprite2D/modulate/a": [0]}})"));
    CHECK(engine.ok("animation.eval", R"({"id": "hero", "animation": "fade", "times": [0.25],
                                          "tracks": ["shadow:Sprite2D/modulate/a"]})") ==
          parse(R"({"length": 0.5, "loop": "none",
                    "tracks": {"shadow:Sprite2D/modulate/a": [0.25]}})"));

    // step plays it: the frame jumps at its key and the event fires as playback passes it.
    engine.ok("events.subscribe");
    CHECK(engine.ok("step", R"({"ticks": 31})").find("tick")->as_int() == 31);
    const json::Value hero = engine.ok("entity.get", R"({"id": "hero"})");
    CHECK(hero.find("components")->find("Sprite2D")->find("frame")->as_int() == 1);
    CHECK(
        hero.find("components")->find("Transform2D")->find("position")->as_array()[1].as_double() ==
        doctest::Approx(-8.0).epsilon(0.01));
    const json::Value events = engine.ok("events.read", R"({"subscription": "events_1"})");
    REQUIRE(events.find("events")->size() == 2);
    CHECK(events.find("events")->as_array()[0] ==
          parse(R"({"data": {"foot": "left"}, "id": "hero", "kind": "footstep", "seq": 1,
                    "tick": 29})"));

    auto reply = engine.fails("animation.eval", R"({"times": [0]})", command::code::kInvalidParams);
    CHECK(contains(reply.message, "give id"));
    reply = engine.fails("animation.eval", R"({"id": "empty", "times": [0]})",
                         command::code::kInvalidParams);
    CHECK(reply.message == "/id: \"empty\" has no AnimationPlayer");
    reply = engine.fails("animation.eval", R"({"id": "hero", "animation": "bop", "times": [0]})",
                         command::code::kNotFound);
    CHECK(reply.message == "/animation: hero.anim.json has no animation \"bop\"; did you mean "
                           "\"bob\"?");
    reply = engine.fails("animation.eval", R"({"path": "hero.anim.json", "times": [0]})",
                         command::code::kInvalidParams);
    CHECK(reply.message ==
          "/animation: name one of hero.anim.json's animations: \"bob\", \"fade\"");
    reply = engine.fails("animation.eval", R"({"id": "hero", "times": [0.5, 2]})",
                         command::code::kInvalidParams);
    CHECK(reply.message == "/times/1: 2 is past the animation's length, 1");
    reply = engine.fails("animation.eval", R"({"id": "hero", "times": [0],
                                              "tracks": ["Sprite2D/fram"]})",
                         command::code::kNotFound);
    CHECK(reply.message == "/tracks/0: bob has no value track \"Sprite2D/fram\"; did you mean "
                           "\"Sprite2D/frame\"?");
    reply = engine.fails("animation.eval", R"({"path": "gone.anim.json", "animation": "a",
                                              "times": [0]})",
                         command::code::kNotFound);
    CHECK(reply.data.find("path")->as_string() == "/path");
    engine.fails("animation.eval", R"({"id": "hero", "times": []})", command::code::kInvalidParams);
    std::filesystem::remove_all(root);
}

namespace {

// A tile set whose grass tiles match by side: water at 0,0, and grass at column 1 + the sides
// facing water (1 n, 2 e, 4 s, 8 w), named for them, as "grass_ns".
std::string sides_tileset() {
    std::string tiles = R"("0,0": {"name": "water", "terrain": "water"})";
    for (int mask = 0; mask < 16; ++mask) {
        std::string name = "grass";
        std::string peering;
        for (int side = 0; side < 4; ++side) {
            if ((mask & (1 << side)) != 0) {
                const std::string letter(1, "nesw"[side]);
                name += (name == "grass" ? "_" : "") + letter;
                peering += (peering.empty() ? "" : ", ") + ("\"" + letter + "\": \"water\"");
            }
        }
        tiles += ", \"" + std::to_string(mask + 1) + ",0\": {\"name\": \"" + name +
                 R"(", "terrain": "grass", "peering": {)" + peering + "}}";
    }
    return R"({"format": 1, "terrain_sets": {"ground": {"mode": "match_sides",
        "terrains": ["grass", "water"]}}, "sources": {"ground": {"texture": "ground.png",
        "tiles": {)" +
           tiles + "}}}}";
}

} // namespace

TEST_CASE("tilemap commands set, paint and read a TileMapLayer's cells") {
    const std::string root = "tilemap_test_project";
    std::filesystem::remove_all(root);
    REQUIRE(slash::platform::create_directories(root).ok());
    REQUIRE(slash::platform::write_file(root + "/ground.tileset.json", sides_tileset()).ok());
    command::SessionOptions options;
    options.project_root = root;
    Engine engine(options);
    engine.ok("entity.create", R"({"id": "map", "components": {
        "TileMapLayer": {"tile_set": "ground.tileset.json"}}})");
    engine.ok("entity.create", R"({"id": "empty", "components": {"Transform2D": {}}})");

    // Painting grass in empty space draws its edges, as if water were all around.
    CHECK(
        engine.ok("tilemap.paint", R"({"id": "map", "rect": [0, 0, 3, 1], "terrain": "grass"})") ==
        parse(R"({"id": "map", "changed": 3})"));
    CHECK(engine.ok("tilemap.get", R"({"id": "map"})") ==
          parse(R"({"origin": [0, 0], "legend": {"G": "grass_ns", "a": "grass_nes",
                    "g": "grass_nsw"}, "rows": ["gGa"]})"));
    // Water below joins up: the grass keeps its edges, now facing water.
    CHECK(engine.ok("tilemap.paint", R"({"id": "map", "cells": [[0, 1], [1, 1], [2, 1]],
                                         "terrain": "water"})") ==
          parse(R"({"id": "map", "changed": 3})"));
    // Grass over the middle water cell turns its neighbors.
    CHECK(engine.ok("tilemap.paint", R"({"id": "map", "cells": [[1, 1]], "terrain": "grass"})") ==
          parse(R"({"id": "map", "changed": 2})"));
    CHECK(engine.ok("tilemap.get", R"({"id": "map"})") ==
          parse(R"({"origin": [0, 0], "legend": {"G": "grass_n", "a": "grass_nes",
                    "b": "grass_esw", "g": "grass_nsw", "w": "water"}, "rows": ["gGa", "wbw"]})"));

    // tilemap.set places one tile, by name or source and grid cell, or erases with null.
    CHECK(engine.ok("tilemap.set", R"({"id": "map", "cells": [[4, -1]], "tile": "ground:1,0"})") ==
          parse(R"({"id": "map", "changed": 1})"));
    CHECK(engine.ok("tilemap.set", R"({"id": "map", "rect": [0, 1, 3, 1], "tile": null})") ==
          parse(R"({"id": "map", "changed": 3})"));
    CHECK(engine.ok("tilemap.get", R"({"id": "map"})") ==
          parse(R"({"origin": [0, -1], "legend": {"G": "grass_n", "a": "grass_nes",
                    "c": "grass", "g": "grass_nsw"}, "rows": ["....c", "gGa"]})"));
    // A window of the layer, in the layer's own keys.
    CHECK(engine.ok("tilemap.get", R"({"id": "map", "rect": [1, -1, 4, 3]})") ==
          parse(R"({"origin": [1, -1], "legend": {"G": "grass_n", "a": "grass_nes",
                    "c": "grass"}, "rows": ["...c", "Ga", ""]})"));

    // Each edit is one undo step, and a dry run changes nothing.
    engine.ok("undo");
    CHECK(engine.ok("tilemap.get", R"({"id": "map", "rect": [0, 1, 3, 1]})")
              .find("rows")
              ->as_array()
              .at(0) == "wbw");
    CHECK(engine.ok("tilemap.set", R"({"id": "map", "cells": [[4, -1]], "tile": "grass"})") ==
          parse(R"({"id": "map", "changed": 0})"));
    engine.ok("tilemap.paint", R"({"id": "map", "rect": [0, 0, 3, 2], "terrain": null,
                                   "dry_run": true})");
    CHECK(engine.ok("tilemap.get", R"({"id": "map"})").find("rows")->size() == 3);

    auto reply = engine.fails("tilemap.set", R"({"id": "nobody", "cells": [[0, 0]], "tile": null})",
                              command::code::kNotFound);
    CHECK(reply.message == "/id: no entity has the id \"nobody\"");
    reply = engine.fails("tilemap.get", R"({"id": "empty"})", command::code::kInvalidParams);
    CHECK(reply.message == "/id: \"empty\" has no TileMapLayer");
    reply = engine.fails("tilemap.set", R"({"id": "map", "cells": [[0, 0]], "tile": "gras"})",
                         command::code::kNotFound);
    CHECK(reply.message ==
          "/tile: the tile set has no tile named \"gras\"; did you mean \"grass\"?");
    reply = engine.fails("tilemap.paint", R"({"id": "map", "cells": [[0, 0]], "terrain": "watr"})",
                         command::code::kNotFound);
    CHECK(reply.message ==
          "/terrain: the tile set has no terrain \"watr\"; did you mean \"water\"?");
    reply = engine.fails("tilemap.paint", R"({"id": "map", "terrain": "water"})",
                         command::code::kInvalidParams);
    CHECK(contains(reply.message, "give cells"));
    reply = engine.fails("tilemap.set", R"({"id": "map", "rect": [0, 0, 300, 300], "tile": null})",
                         command::code::kInvalidParams);
    CHECK(reply.message == "/rect: covers 90000 cells; the most is 65536");
    engine.fails("tilemap.set", R"({"id": "map", "cells": [[0, 40000]], "tile": null})",
                 command::code::kInvalidParams);
    engine.ok("component.set", R"({"id": "map", "components": {"TileMapLayer": {"rows": ["q"]}}})");
    reply = engine.fails("tilemap.get", R"({"id": "map"})", command::code::kInvalidArgument);
    CHECK(reply.message == "/id: \"map\"'s TileMapLayer/rows/0: column 0: \"q\" is not in the "
                           "legend");
    std::filesystem::remove_all(root);
}

TEST_CASE("log.tail returns recent log lines as objects") {
    Engine engine({}, /*capture_logs=*/true);
    slash::log::info("first", {{"n", 1}});
    slash::log::warn("second");
    slash::log::error("third");
    const json::Value last = engine.ok("log.tail", R"({"lines": 2})");
    REQUIRE(last.find("lines")->size() == 2);
    CHECK(last.find("lines")->as_array()[0].find("msg")->as_string() == "second");
    CHECK(last.find("lines")->as_array()[1].find("msg")->as_string() == "third");

    const json::Value errors = engine.ok("log.tail", R"({"level": "error"})");
    REQUIRE(errors.find("lines")->size() == 1);
    CHECK(errors.find("lines")->as_array()[0].find("msg")->as_string() == "third");
}

TEST_CASE("audio commands play players, report buses and capture what was heard") {
    const std::string root = "command_audio_project";
    REQUIRE(slash::platform::create_directories(root).ok());
    const std::vector<slash::audio::Frame> hum(44100, slash::audio::Frame{0.5F, 0.5F});
    REQUIRE(
        slash::platform::write_file(root + "/hum.wav", slash::audio::encode_wav(hum, 44100)).ok());
    command::SessionOptions options;
    options.project_root = root;
    Engine engine(options);
    engine.ok("entity.create",
              R"({"id": "music", "components": {"AudioStreamPlayer": {"stream": "hum.wav"}}})");
    engine.ok("entity.create", R"({"id": "rock"})");

    CHECK(engine.ok("audio.state") == parse(R"({"mix_rate": 44100,
        "buses": [{"name": "Master", "volume_db": 0, "peak_db": null}], "players": []})"));
    CHECK(engine.ok("audio.play", R"({"id": "music"})") == parse(R"({"tick": 0})"));
    engine.ok("step", R"({"ticks": 3})");
    json::Value state = engine.ok("audio.state");
    CHECK(*state.find("buses") == parse(R"([{"name": "Master", "volume_db": 0, "peak_db": -6}])"));
    const json::Value& player = state.find("players")->as_array().at(0);
    CHECK(player.find("id")->as_string() == "music");
    CHECK(player.find("kind")->as_string() == "AudioStreamPlayer");
    CHECK(player.find("voices")->as_int() == 1);
    CHECK(player.find("length")->as_double() == 1.0);
    CHECK(player.find("position")->as_double() > 0.04);
    CHECK(*player.find("volume") == parse("[1, 1]"));
    CHECK(engine.ok("audio.state", R"({"ids": ["other"]})").find("players")->size() == 0);

    // Peak and RMS in dB to a tenth, and the peak of each slice of the time asked for.
    CHECK(engine.ok("audio.capture", R"({"seconds": 0.02, "slices": 2})") ==
          parse(R"({"seconds": 0.02, "peak_db": -6, "rms_db": -6, "slices": [-6, -6]})"));
    engine.fails("audio.capture", R"({"bus": "Mastr"})", command::code::kNotFound);
    engine.fails("audio.capture", R"({"path": "capture.mp3"})", command::code::kInvalidArgument);
    const json::Value written = engine.ok("audio.capture", R"({"path": "out/capture.wav"})");
    CHECK(written.find("path")->as_string() == "out/capture.wav");
    const auto wav = slash::platform::read_file(root + "/out/capture.wav");
    REQUIRE(wav.ok());
    auto heard = slash::audio::decode_sound(wav.value(), "capture.wav");
    REQUIRE(heard.ok());
    CHECK(heard.value().channels == 2);
    CHECK(heard.value().frames > 2000);

    // Bus changes last until the layout is loaded again; a failed batch puts them back.
    CHECK(engine.ok("audio.set_bus", R"({"bus": "Master", "volume_db": -12, "mute": true})") ==
          parse(R"({"name": "Master", "volume_db": -12, "peak_db": -6, "mute": true})"));
    const auto typo =
        engine.fails("audio.set_bus", R"({"bus": "Mastre"})", command::code::kNotFound);
    CHECK(contains(typo.message, "did you mean \"Master\""));
    engine.fails("audio.set_bus", R"({"bus": "Master", "effect": 0, "enabled": true})",
                 command::code::kInvalidArgument);
    engine.fails("audio.set_bus", R"({"bus": "Master", "enabled": true})",
                 command::code::kInvalidParams);
    engine.call("batch", R"({"commands": [
        {"command": "audio.set_bus", "params": {"bus": "Master", "volume_db": 0, "mute": false}},
        {"command": "entity.destroy", "params": {"id": "nobody"}}]})");
    CHECK(engine.ok("audio.state").find("buses")->as_array().at(0).find("mute") != nullptr);
    engine.ok("audio.set_bus", R"({"bus": "Master", "volume_db": 0, "mute": false})");

    CHECK(engine.ok("audio.stop", R"({"id": "music"})") == parse(R"({"tick": 3})"));
    engine.ok("step");
    CHECK(engine.ok("audio.state").find("players")->as_array().at(0).find("voices")->as_int() == 0);
    const auto rock =
        engine.fails("audio.play", R"({"id": "rock"})", command::code::kInvalidParams);
    CHECK(contains(rock.message, "has no AudioStreamPlayer or AudioStreamPlayer2D"));
    engine.fails("audio.play", R"({"id": "musik"})", command::code::kNotFound);

    CHECK(engine.ok("schema.get", R"({"kind": "bus_layout"})").find("title")->as_string() ==
          "SlashEngine bus layout");
    CHECK(engine.ok("schema.get", R"({"kind": "audio"})").find("title")->as_string() ==
          "SlashEngine audio stream");

    // Writing a capture needs the filesystem capability.
    options.capabilities = {Capability::Read, Capability::Run};
    Engine reader(options);
    reader.fails("audio.capture", R"({"path": "x.wav"})", command::code::kPermissionDenied);
    CHECK(reader.ok("audio.capture").find("seconds")->as_double() == 0.0);
}

TEST_CASE("the call listener hears each command that ran, with the tick it started at") {
    Engine engine;
    std::vector<std::string> heard;
    engine.session->set_call_listener(
        [&](std::string_view name, const json::Value& params, std::uint64_t tick) {
            heard.push_back(std::to_string(tick) + " " + std::string(name) + " " +
                            json::write(params, json::Style::Compact));
        });
    engine.ok("entity.create", R"({"id": "a"})");
    engine.ok("step", R"({"ticks": 3})");
    engine.ok("entity.get", R"({"id": "a"})");
    // Not failures, dry runs or the commands inside a batch.
    engine.fails("entity.get", R"({"id": "b"})", command::code::kNotFound);
    engine.ok("entity.create", R"({"id": "b", "dry_run": true})");
    engine.ok("batch", R"({"commands": [{"command": "entity.destroy", "params": {"id": "a"}}]})");
    CHECK(heard ==
          std::vector<std::string>{
              R"(0 entity.create {"id":"a"})", R"(0 step {"ticks":3})",
              R"(3 entity.get {"id":"a"})",
              R"(3 batch {"commands":[{"command":"entity.destroy","params":{"id":"a"}}]})"});
    engine.session->set_call_listener({});
    engine.ok("step");
    CHECK(heard.size() == 4);
}

TEST_CASE("the session's revision counts the calls that may change the world") {
    Engine engine;
    const std::uint64_t start = engine.session->revision();
    engine.ok("entity.create", R"({"id": "a"})");
    engine.ok("entity.get", R"({"id": "a"})");
    engine.ok("world.query", R"({"with": ["Transform2D"]})");
    CHECK(engine.session->revision() == start + 1);
    // Failures, dry runs and undo count too, as they may have stepped or changed state outside
    // the components; the commands inside a batch count as the batch.
    engine.fails("entity.destroy", R"({"id": "b"})", command::code::kNotFound);
    engine.ok("entity.create", R"({"id": "b", "dry_run": true})");
    engine.ok("undo");
    engine.ok("batch", R"({"commands": [{"command": "entity.create", "params": {"id": "c"}},
                                        {"command": "step"}]})");
    CHECK(engine.session->revision() == start + 5);
}
