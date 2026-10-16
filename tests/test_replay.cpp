#include "slash/command/builtin.hpp"
#include "slash/command/command.hpp"
#include "slash/command/replay.hpp"
#include "slash/command/session.hpp"
#include "slash/core/json.hpp"
#include "slash/core/json_schema.hpp"
#include "slash/input/input.hpp"
#include "slash/platform/file.hpp"
#include "slash/world/component_registry.hpp"
#include "slash/world/components.hpp"

#include <doctest/doctest.h>

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace json = slash::json;
namespace world = slash::world;
namespace command = slash::command;

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

    explicit Engine(command::SessionOptions options = {}) {
        world::register_builtin_components(components);
        command::register_builtin_commands(commands);
        options.capture_logs = false;
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

    void steps(int ticks, double seconds = 1.0 / 60.0) {
        for (int i = 0; i < ticks; ++i) {
            session->world().step(seconds);
        }
    }
};

// A project with a floor, two crates from a prefab and a ball, and a run recorded over it.
struct Recorded {
    std::string root = "replay_project";
    command::SessionOptions options;
    json::Value at_30;   // the entities at tick 30
    json::Value end;     // the entities when the recording stopped
    json::Value stopped; // replay.stop's reply
    std::uint64_t end_tick = 0;
    std::unique_ptr<Engine> recorder; // the session that recorded it

    Recorded() {
        std::filesystem::remove_all(root);
        REQUIRE(slash::platform::create_directories(root + "/prefabs").ok());
        REQUIRE(slash::platform::write_file(root + "/project.json", R"({"format": 1,
            "name": "Replay", "main_scene": "main.scene.json", "version": "1.0.0",
            "window": {"width": 320, "height": 180}})")
                    .ok());
        REQUIRE(slash::platform::write_file(root + "/prefabs/crate.prefab.json", R"({"format": 1,
            "components": {"RigidBody2D": {"contact_monitor": true},
                           "CollisionShape2D": {"size": [24, 24]}}})")
                    .ok());
        options.project_root = root;
        recorder = std::make_unique<Engine>(options);
        Engine& engine = *recorder;
        // Entities come and go first, so their handles differ from a new session's.
        for (int i = 0; i < 20; ++i) {
            engine.ok("entity.create", R"({"id": "junk"})");
            engine.ok("entity.destroy", R"({"id": "junk"})");
        }
        engine.ok("batch", R"({"commands": [
            {"command": "entity.create", "params": {"id": "floor", "components": {
                "Transform2D": {"position": [160, 170]}, "StaticBody2D": {},
                "CollisionShape2D": {"size": [320, 20]}}}},
            {"command": "entity.create", "params": {"id": "crate_a",
                "extends": "prefabs/crate.prefab.json",
                "components": {"Transform2D": {"position": [100, 40]}}}},
            {"command": "entity.create", "params": {"id": "crate_b",
                "extends": "prefabs/crate.prefab.json",
                "components": {"Transform2D": {"position": [110, 0]}}}},
            {"command": "entity.create", "params": {"id": "ball", "components": {
                "Transform2D": {"position": [200, 20]}, "RigidBody2D": {"bounce": 0.5},
                "CollisionShape2D": {"shape": "circle", "radius": 8}}}}]})");

        CHECK(engine.ok("replay.record", R"({"path": "replays/run.replay.json"})") ==
              parse(R"({"recording": "replays/run.replay.json", "tick": 0})"));
        world::World& game = engine.session->world();
        engine.steps(30);
        at_30 = engine.scene();
        slash::input::action_press(game, "ui_accept");
        engine.steps(10);
        engine.ok(
            "component.set",
            R"({"id": "ball", "components": {"RigidBody2D": {"linear_velocity": [-120, 0]}}})");
        slash::input::action_release(game, "ui_accept");
        engine.steps(20, 1.0 / 120.0);
        engine.ok("step", R"({"ticks": 10})");
        engine.ok("entity.destroy", R"({"id": "crate_b"})");
        engine.ok("undo");
        engine.ok("world.snapshot", R"({"name": "mid"})");
        engine.steps(15);
        engine.ok("world.restore", R"({"name": "mid"})");
        // Neither reads, dry runs nor failures are kept.
        engine.ok("world.query", R"({"with": ["RigidBody2D"]})");
        engine.ok("entity.destroy", R"({"id": "ball", "dry_run": true})");
        engine.fails("entity.destroy", R"({"id": "nobody"})", command::code::kNotFound);
        engine.steps(70);
        end_tick = game.tick();
        end = engine.scene();
        stopped = engine.ok("replay.stop");
        CHECK_FALSE(command::recording(*engine.session));
    }

    json::Value file() const {
        auto text = slash::platform::read_file(root + "/replays/run.replay.json");
        REQUIRE(text.ok());
        return parse(text.value());
    }

    void write(std::string_view name, const json::Value& replay) const {
        REQUIRE(
            slash::platform::write_file(root + "/replays/" + std::string(name), json::write(replay))
                .ok());
    }
};

} // namespace

TEST_CASE("a recorded run plays back exactly, in its own session and in a new one") {
    Recorded run;
    CHECK(run.end_tick == 155);
    CHECK(run.stopped == parse(R"({"path": "replays/run.replay.json", "ticks": 155,
                                   "commands": 5, "inputs": 2})"));
    // Things moved: the crates fell and the ball rolled.
    CHECK(run.end.find("crate_a")->find("components")->find("Transform2D") !=
          run.at_30.find("crate_a")->find("components")->find("Transform2D"));

    // The file is valid, holds the runs, input, commands and checks, and keeps a copy of the
    // prefab, so it plays back without the file.
    const json::Value replay = run.file();
    CHECK(command::validate_replay(replay).empty());
    CHECK(*replay.find("steps") == parse(R"([
        {"tick": 0, "count": 40, "seconds": 0.016666666666666666},
        {"tick": 40, "count": 20, "seconds": 0.008333333333333333},
        {"tick": 70, "count": 85, "seconds": 0.016666666666666666}])"));
    std::vector<std::string> kinds;
    for (const json::Value& entry : replay.find("timeline")->as_array()) {
        const json::Value* name = entry.find("command");
        kinds.push_back(std::to_string(entry.find("tick")->as_int()) + " " +
                        (name != nullptr ? name->as_string()
                                         : (entry.find("input") != nullptr ? "input" : "?")) +
                        (entry.find("world") != nullptr ? " (world)" : ""));
    }
    CHECK(kinds == std::vector<std::string>{"30 input", "40 component.set", "40 input", "60 step",
                                            "70 entity.destroy", "70 undo (world)",
                                            "85 world.restore (world)"});
    CHECK(*replay.find("checks")->as_array()[0].find("tick") == json::Value(120));
    CHECK(replay.find("prefabs")->find("prefabs/crate.prefab.json") != nullptr);
    std::filesystem::remove(run.root + "/prefabs/crate.prefab.json");

    const json::Value matched = parse(R"({"tick": 155, "end": 155, "matched": true})");
    SUBCASE("in the session that recorded it, twice") {
        Engine& engine = *run.recorder;
        CHECK(engine.ok("replay.run", R"({"path": "replays/run.replay.json"})") == matched);
        CHECK(engine.scene() == run.end);
        CHECK(engine.ok("replay.run", R"({"path": "replays/run.replay.json"})") == matched);
        CHECK(engine.scene() == run.end);
    }
    SUBCASE("in a new session") {
        Engine engine(run.options);
        CHECK(engine.ok("replay.run", R"({"path": "replays/run.replay.json"})") == matched);
        CHECK(engine.scene() == run.end);
        CHECK(engine.ok("history.get") == parse(R"({"undo": [], "redo": []})"));
    }
    SUBCASE("up to a tick") {
        Engine engine(run.options);
        CHECK(engine.ok("replay.run", R"({"path": "replays/run.replay.json", "tick": 30})") ==
              parse(R"({"tick": 30, "end": 155, "matched": true})"));
        CHECK(engine.scene() == run.at_30);
        // A tick inside a command's steps stops where the command did.
        CHECK(engine.ok("replay.run", R"({"path": "replays/run.replay.json", "tick": 65})") ==
              parse(R"({"tick": 70, "end": 155, "matched": true})"));
    }
    std::filesystem::remove_all(run.root);
}

TEST_CASE("replay.run says where a run went another way") {
    Recorded run;
    Engine engine(run.options);

    // A command with other params: its hash differs at once. Restoring a snapshot later puts
    // the recorded entities back, so it still ends the same.
    json::Value changed = run.file();
    json::Value& set = changed.find("timeline")->as_array()[1];
    (*set.find("params"))["components"]["RigidBody2D"]["linear_velocity"] = json::Array{-60, 0};
    run.write("changed.replay.json", changed);
    CHECK(engine.ok("replay.run", R"({"path": "replays/changed.replay.json"})") ==
          parse(R"({"tick": 155, "end": 155, "matched": false, "diverged": 40})"));

    // Longer ticks after the last restore: the check at 120 differs, and so does the end.
    json::Value slower = run.file();
    (*slower.find("steps")->as_array()[2].find("seconds")) = 1.0 / 30.0;
    run.write("slower.replay.json", slower);
    const json::Value result = engine.ok("replay.run", R"({"path": "replays/slower.replay.json"})");
    CHECK(*result.find("matched") == json::Value(false));
    CHECK(*result.find("diverged") == json::Value(120));
    CHECK(result.find("changes")->find("changed")->find("ball") != nullptr);

    // A command that fails stops the replay there.
    json::Value failing = run.file();
    (*failing.find("timeline")->as_array()[1].find("params"))["id"] = "nobody";
    run.write("failing.replay.json", failing);
    const json::Value stopped =
        engine.ok("replay.run", R"({"path": "replays/failing.replay.json"})");
    CHECK(*stopped.find("tick") == json::Value(40));
    CHECK(*stopped.find("diverged") == json::Value(40));
    CHECK(stopped.find("failed")->find("command")->as_string() == "component.set");
    std::filesystem::remove_all(run.root);
}

TEST_CASE("replay files are checked before they play") {
    Recorded run;
    const json::Value good = run.file();
    const auto errors_of = [](const json::Value& replay) {
        std::vector<std::string> out;
        for (const json::ValidationError& error : command::validate_replay(replay)) {
            out.push_back(error.path + ": " + error.message);
        }
        return out;
    };

    json::Value overlapping = good;
    (*overlapping.find("steps")->as_array()[1].find("tick")) = 30;
    CHECK(
        errors_of(overlapping) ==
        std::vector<std::string>{"/steps/1/tick: comes before tick 40, where the run before ends"});

    json::Value unordered = good;
    (*unordered.find("timeline")->as_array()[0].find("tick")) = 50;
    CHECK(errors_of(unordered) ==
          std::vector<std::string>{"/timeline/1/tick: comes before tick 50, where the run "
                                   "already was",
                                   "/timeline/2/tick: comes before tick 50, where the run "
                                   "already was"});

    json::Value mixed = good;
    mixed.find("timeline")->as_array()[0]["viewport"] = json::Array{100, 100};
    CHECK(errors_of(mixed) ==
          std::vector<std::string>{"/timeline/0: an entry has one of input, command or viewport"});

    json::Value bad_event = good;
    json::Value& event = bad_event.find("timeline")->as_array()[0].find("input")->as_array()[0];
    event["presed"] = true;
    const std::vector<std::string> event_errors = errors_of(bad_event);
    REQUIRE(event_errors.size() == 1);
    CHECK(event_errors[0].starts_with("/timeline/0/input/0"));

    json::Value bad_hash = good;
    (*bad_hash.find("checks")->as_array()[0].find("hash")) = "0123456789ABCDEF";
    CHECK(errors_of(bad_hash) ==
          std::vector<std::string>{"/checks/0/hash: is 16 lowercase hex digits"});

    json::Value schema_error = good;
    schema_error.as_object().erase("end");
    CHECK_FALSE(errors_of(schema_error).empty());

    // replay.run reports every error with the file's JSON Pointers.
    run.write("bad.replay.json", overlapping);
    Engine engine(run.options);
    const command::Reply reply = engine.fails(
        "replay.run", R"({"path": "replays/bad.replay.json"})", command::code::kInvalidArgument);
    CHECK(reply.message == "replays/bad.replay.json is not a valid replay: /steps/1/tick: comes "
                           "before tick 40, where the run before ends");
    CHECK(*reply.data.find("file") == json::Value("replays/bad.replay.json"));
    std::filesystem::remove_all(run.root);
}

TEST_CASE("replay commands refuse what would lose or mix up a recording") {
    const std::string root = "replay_errors_project";
    std::filesystem::remove_all(root);
    REQUIRE(slash::platform::create_directories(root).ok());
    command::SessionOptions options;
    options.project_root = root;
    Engine engine(options);

    CHECK(engine.fails("replay.stop", "{}", command::code::kInvalidArgument).message ==
          "nothing is recording; start with replay.record");
    CHECK(engine.fails("replay.record", R"({"path": "run.json"})", command::code::kInvalidArgument)
              .message ==
          R"(/path: replay files end in ".replay.json", e.g. "replays/run.replay.json")");
    CHECK(engine
              .fails("replay.record", R"({"path": "../run.replay.json"})",
                     command::code::kPermissionDenied)
              .message.starts_with("/path: "));
    CHECK(engine.fails("replay.run", R"({"path": "missing.replay.json"})", command::code::kNotFound)
              .message.starts_with("/path: "));

    engine.ok("replay.record", R"({"path": "run.replay.json"})");
    CHECK(engine
              .fails("replay.record", R"({"path": "other.replay.json"})",
                     command::code::kInvalidArgument)
              .message == "/path: already recording to run.replay.json; stop it with replay.stop");
    CHECK(
        engine
            .fails("replay.run", R"({"path": "run.replay.json"})", command::code::kInvalidArgument)
            .message == "recording to run.replay.json; stop it with replay.stop first");
    CHECK(engine
              .fails("batch", R"({"commands": [{"command": "replay.stop"}]})",
                     command::code::kInvalidArgument)
              .message.ends_with("replay.stop can't run inside a batch"));
    engine.ok("entity.create", R"({"id": "a"})");
    engine.steps(5);
    CHECK(engine.ok("replay.stop") ==
          parse(R"({"path": "run.replay.json", "ticks": 5, "commands": 1, "inputs": 0})"));
    CHECK(engine
              .fails("replay.run", R"({"path": "run.replay.json", "tick": 6})",
                     command::code::kInvalidArgument)
              .message == "/tick: the replay runs from tick 0 to 5");

    // schema.get serves the file format.
    CHECK(engine.ok("schema.get", R"({"kind": "replay"})") == command::replay_schema());
    std::filesystem::remove_all(root);
}
