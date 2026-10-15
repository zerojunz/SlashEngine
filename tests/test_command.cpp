#include "slash/command/builtin.hpp"
#include "slash/command/command.hpp"
#include "slash/command/session.hpp"
#include "slash/core/json.hpp"
#include "slash/core/json_schema.hpp"
#include "slash/core/log.hpp"
#include "slash/platform/file.hpp"
#include "slash/world/component_registry.hpp"
#include "slash/world/components.hpp"

#include <doctest/doctest.h>

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
          parse(R"({"position": [0, 0], "rotation": 1.5, "scale": [1, 1]})"));

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

    // Each command reads prefab files again, so edits show up in new instances.
    REQUIRE(slash::platform::write_file(root + "/prefabs/enemy.prefab.json",
                                        R"({"format": 1, "components": {
                                            "Health": {"current": 80, "max": 80}}})")
                .ok());
    engine.ok("entity.create", R"({"id": "fresh", "extends": "prefabs/enemy.prefab.json"})");
    CHECK(engine.ok("entity.get", R"({"id": "fresh"})").find("components")->size() == 1);

    // Undo restores instances from the prefab they were made with, without reading the file.
    std::filesystem::remove(root + "/prefabs/enemy.prefab.json");
    engine.ok("entity.destroy", R"({"id": "grunt"})");
    engine.ok("undo");
    CHECK(engine.ok("entity.get", R"({"id": "grunt", "omit_defaults": true})") == parse(R"({
        "id": "grunt", "extends": "prefabs/enemy.prefab.json",
        "components": {"Health": {"current": 20.0, "max": 50.0},
                       "Transform2D": {"rotation": 1.0}}})"));
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

TEST_CASE("engine.describe lists systems in the order they run") {
    Engine engine;
    world::World& world = engine.session->world();
    world.add_system("movement", [](world::World& /*w*/, double /*dt*/) {},
                     {.doc = "Moves things by their velocity."});
    world.add_system("input", [](world::World& /*w*/, double /*dt*/) {}, {.priority = -1});
    CHECK(*engine.ok("engine.describe").find("systems") == parse(R"(["input", "movement"])"));
    CHECK(*engine.ok("engine.describe", R"({"full": true})").find("systems") == parse(R"([
        {"name": "input", "priority": -1},
        {"name": "movement", "doc": "Moves things by their velocity.", "priority": 0}
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

TEST_CASE("schema.get returns scene, component and command schemas") {
    Engine engine;
    CHECK(engine.ok("schema.get", R"({"kind": "prefab"})").find("title")->as_string() ==
          "SlashEngine prefab");
    CHECK(engine.ok("schema.get", R"({"kind": "scene"})").find("title")->as_string() ==
          "SlashEngine scene");

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
