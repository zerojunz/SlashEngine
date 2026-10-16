#include "slash/command/builtin.hpp"
#include "slash/command/command.hpp"
#include "slash/command/session.hpp"
#include "slash/command/test.hpp"
#include "slash/core/json.hpp"
#include "slash/core/json_schema.hpp"
#include "slash/core/log.hpp"
#include "slash/platform/file.hpp"
#include "slash/world/component_registry.hpp"
#include "slash/world/components.hpp"

#include <doctest/doctest.h>

#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
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

std::string compact(const json::Value& value) {
    return json::write(value, json::Style::Compact);
}

// A project folder with a floor, a crate that falls onto it and a second scene, and a session
// over it.
struct Project {
    std::string root;
    world::ComponentRegistry components;
    command::Registry commands;
    std::unique_ptr<command::Session> session;

    explicit Project(std::string folder,
                     const std::function<void(command::Registry&)>& add_commands = {})
        : root(std::move(folder)) {
        std::filesystem::remove_all(root);
        REQUIRE(slash::platform::create_directories(root + "/tests").ok());
        write("project.json", R"({"format": 1, "name": "Tests", "main_scene": "main.scene.json",
            "version": "1.0.0", "window": {"width": 320, "height": 180}})");
        write("main.scene.json", R"({"format": 1, "entities": {
            "crate": {"components": {"Transform2D": {"position": [160, 40]}, "RigidBody2D": {},
                                     "CollisionShape2D": {"size": [16, 16]}}},
            "floor": {"components": {"Transform2D": {"position": [160, 170]},
                                     "StaticBody2D": {},
                                     "CollisionShape2D": {"size": [320, 20]}}}}})");
        write("other.scene.json", R"({"format": 1, "entities": {"other": {}}})");
        world::register_builtin_components(components);
        command::register_builtin_commands(commands);
        if (add_commands) {
            add_commands(commands);
        }
        command::SessionOptions options;
        options.project_root = root;
        options.capture_logs = false;
        session = std::make_unique<command::Session>(components, commands, options);
    }

    void write(const std::string& path, std::string_view text) const {
        REQUIRE(slash::platform::write_file(root + "/" + path, text).ok());
    }

    [[nodiscard]] bool exists(const std::string& path) const {
        return slash::platform::exists(root + "/" + path);
    }

    command::Reply call(std::string_view name, std::string_view params = "{}") const {
        return session->call(name, parse(params));
    }

    json::Value run(std::string_view params = "{}") const {
        command::Reply reply = call("test.run", params);
        REQUIRE_MESSAGE(reply.ok, reply.message);
        return std::move(reply.value);
    }

    // The failures of a run of one file, as compact JSON lines without the file.
    std::vector<std::string> failures(const std::string& file) const {
        const json::Value result = run(R"({"paths": [")" + file + R"("]})");
        std::vector<std::string> lines;
        for (json::Value item : result.find("failed")->as_array()) {
            item.as_object().erase("file");
            lines.push_back(compact(item));
        }
        return lines;
    }
};

} // namespace

TEST_CASE("test.run runs each test from its scene and returns only the checks that failed") {
    const Project project("test_run_project");
    project.write("tests/physics.test.json", R"({"scene": "main.scene.json", "tests": {
        "crate_sleeps": {"steps": [
            {"command": "run.until", "params": {"where": [
                {"component": "RigidBody2D", "field": "sleeping", "value": true}], "ticks": 600},
             "expect": {"matched": true, "entities": ["crate"]}},
            {"command": "world.query", "params": {"where": [
                {"component": "Transform2D", "field": "position.y", "op": ">", "value": 150}]},
             "expect": {"entities": ["crate", "floor"]}}]},
        "starts_afresh": {"steps": [
            {"command": "step", "params": {"ticks": 10}, "expect": {"tick": 10}}]},
        "two_crates": {"steps": [
            {"command": "step"},
            {"command": "world.query", "params": {"with": ["RigidBody2D"]},
             "expect": {"count": 2}},
            {"command": "step", "params": {"ticks": 5}}]},
        "other_scene": {"scene": "other.scene.json", "steps": [
            {"command": "world.query", "expect": {"entities": ["other"]}}]}}})");

    // The session's own world stays as it was.
    REQUIRE(project.call("entity.create", R"({"id": "mine"})").ok);
    const json::Value result = project.run();
    CHECK(compact(result) ==
          R"({"failed":[{"file":"tests/physics.test.json","message":"expected 2, got 1",)"
          R"("path":"/tests/two_crates/steps/1/expect/count","test":"two_crates","tick":1}],)"
          R"("passed":3,"tests":4})");
    CHECK(project.session->world().tick() == 0);
    CHECK(compact(project.call("world.query").value.find("entities")->as_array()) == R"(["mine"])");
    CHECK(project.session->undo_labels() == std::vector<std::string>{"entity.create"});

    // Every test passing is three numbers and an empty list.
    project.write("tests/physics.test.json", R"({"scene": "main.scene.json", "tests": {
        "starts_afresh": {"steps": [{"command": "step", "expect": {"tick": 1}}]}}})");
    CHECK(compact(project.run()) == R"({"failed":[],"passed":1,"tests":1})");
}

TEST_CASE("test.run points a failed command at the bad value in the test file") {
    const Project project("test_run_commands");
    project.write("tests/errors.test.json", R"({"scene": "main.scene.json", "tests": {
        "missing_entity": {"steps": [{"command": "entity.get", "params": {"id": "crat"}},
                                     {"command": "entity.get", "params": {"id": "nobody"}}]},
        "misspelled_command": {"steps": [{"command": "wrold.query"}]},
        "bad_params": {"steps": [{"command": "step", "params": {"ticks": "ten"}}]},
        "in_a_batch": {"steps": [{"command": "batch", "params": {"commands": [
            {"command": "entity.create", "params": {"id": "box"}},
            {"command": "entity.destroy", "params": {"id": "bxo"}}]}}]},
        "missing_scene": {"scene": "lost.scene.json", "steps": [{"command": "step"}]}}})");
    const std::vector<std::string> failures = project.failures("tests/errors.test.json");
    REQUIRE(failures.size() == 5);
    CHECK(failures[0] == R"({"message":"expected integer, got string",)"
                         R"("path":"/tests/bad_params/steps/0/params/ticks",)"
                         R"("test":"bad_params","tick":0})");
    CHECK(failures[1] == R"({"message":"/commands/1 (entity.destroy) failed, so nothing was )"
                         R"(changed. /id: no entity has the id \"bxo\"; did you mean \"box\"?",)"
                         R"("path":"/tests/in_a_batch/steps/0/params/commands/1/params/id",)"
                         R"("test":"in_a_batch","tick":0})");
    // A test stops at its first failed check.
    CHECK(failures[2] == R"({"message":"no entity has the id \"crat\"; did you mean \"crate\"?",)"
                         R"("path":"/tests/missing_entity/steps/0/params/id",)"
                         R"("test":"missing_entity","tick":0})");
    CHECK(failures[3].starts_with(R"({"message":")"));
    CHECK(failures[3].find(R"("path":"/tests/missing_scene/scene")") != std::string::npos);
    CHECK(failures[4] == R"({"message":"unknown command \"wrold.query\"; did you mean )"
                         R"(\"world.query\"?","path":"/tests/misspelled_command/steps/0/command",)"
                         R"("test":"misspelled_command","tick":0})");
}

TEST_CASE("test.run matches replies against what steps expect") {
    const Project project("test_run_expect");
    project.write("many.scene.json", R"({"format": 1, "entities": {
        "a_rather_long_entity_name_0": {}, "a_rather_long_entity_name_1": {},
        "a_rather_long_entity_name_2": {}}})");
    project.write("tests/expect.test.json", R"({"scene": "main.scene.json", "tests": {
        "numbers_by_value": {"steps": [{"command": "world.query",
                                        "expect": {"count": 2.0, "truncated": false}}]},
        "only_named_members": {"steps": [{"command": "entity.get", "params": {"id": "crate"},
            "expect": {"components": {"Transform2D": {"position": [160, 40]}}}}]},
        "missing_member": {"steps": [{"command": "world.query", "expect": {"a/b": 1}}]},
        "wrong_type": {"steps": [{"command": "world.query", "expect": {"entities": "crate"}}]},
        "array_length": {"steps": [{"command": "world.query", "expect": {"entities": ["crate"]}}]},
        "long_values": {"scene": "many.scene.json", "steps": [
            {"command": "world.query", "expect": {"entities": []}}]},
        "array_item": {"steps": [{"command": "world.query",
                                  "expect": {"entities": ["crate", "flor"]}}]}}})");
    const std::vector<std::string> failures = project.failures("tests/expect.test.json");
    REQUIRE(failures.size() == 5);
    CHECK(failures[0] == R"({"message":"expected \"flor\", got \"floor\"",)"
                         R"("path":"/tests/array_item/steps/0/expect/entities/1",)"
                         R"("test":"array_item","tick":0})");
    CHECK(failures[1] == R"({"message":"expected 1 item, got 2 items: [\"crate\",\"floor\"]",)"
                         R"("path":"/tests/array_length/steps/0/expect/entities",)"
                         R"("test":"array_length","tick":0})");
    // Long values are cut short.
    CHECK(failures[2] ==
          R"({"message":"expected 0 items, got 3 items: )"
          R"([\"a_rather_long_entity_name_0\",\"a_rather_long_entity_name...",)"
          R"("path":"/tests/long_values/steps/0/expect/entities","test":"long_values","tick":0})");
    CHECK(failures[3] == R"({"message":"missing; expected 1",)"
                         R"("path":"/tests/missing_member/steps/0/expect/a~1b",)"
                         R"("test":"missing_member","tick":0})");
    CHECK(failures[4] == R"({"message":"expected \"crate\", got [\"crate\",\"floor\"]",)"
                         R"("path":"/tests/wrong_type/steps/0/expect/entities",)"
                         R"("test":"wrong_type","tick":0})");
}

TEST_CASE("a warning or an error logged while a test runs fails it") {
    const Project project("test_run_logs", [](command::Registry& registry) {
        registry.add({
            .name = "game.lose_crate",
            .description = "Logs an error, as gameplay code would.",
            .capabilities = {command::Capability::Run},
            .effect = command::Effect::Read,
            .params = parse(R"({"type": "object"})"),
            .handler =
                [](command::Session& /*session*/, const json::Value& /*params*/) {
                    slash::log::error("the crate fell out of the world", {{"id", "crate"}});
                    return command::Reply::success(json::Object{});
                },
        });
    });
    project.write("tests/logs.test.json", R"({"scene": "main.scene.json", "tests": {
        "error": {"steps": [{"command": "step"}, {"command": "game.lose_crate"},
                            {"command": "step"}]},
        "warning": {"steps": [
            {"command": "entity.create", "params": {"id": "torch", "components": {
                "AnimatedSprite2D": {"sprite_frames": "torch.frames.json"}}}},
            {"command": "step", "params": {"ticks": 3}}]},
        "quiet": {"steps": [{"command": "step", "params": {"ticks": 3}}]}}})");

    // Lines still reach the sink that was there, which is back afterwards.
    std::vector<std::string> lines;
    slash::log::set_sink(
        [&](slash::log::Level /*level*/, std::string_view line) { lines.emplace_back(line); });
    const json::Value result = project.run(R"({"paths": ["tests"]})");
    slash::log::write(slash::log::Level::Warn, "after the tests");
    slash::log::reset_sink();

    CHECK(result.find("passed")->as_int() == 1);
    const json::Array& failed = result.find("failed")->as_array();
    REQUIRE(failed.size() == 2);
    CHECK(compact(failed[0]) ==
          R"({"file":"tests/logs.test.json","log":{"id":"crate","level":"error",)"
          R"("msg":"the crate fell out of the world"},)"
          R"("message":"logged an error: the crate fell out of the world",)"
          R"("path":"/tests/error/steps/1","test":"error","tick":1})");
    CHECK(failed[1].find("message")->as_string() ==
          "logged a warning: an AnimatedSprite2D can't play");
    CHECK(failed[1].find("path")->as_string() == "/tests/warning/steps/1");
    CHECK(failed[1].find("log")->find("id")->as_string() == "torch");
    CHECK(failed[1].find("tick")->as_int() == 3); // when the step ended
    REQUIRE(lines.size() >= 3);
    CHECK(lines[lines.size() - 3].find("the crate fell out of the world") != std::string::npos);
    CHECK(lines[lines.size() - 2].find("an AnimatedSprite2D can't play") != std::string::npos);
    CHECK(lines.back().find("after the tests") != std::string::npos);
}

TEST_CASE("tests can't write files or run tests") {
    const Project project("test_run_sandbox");
    project.write("tests/sandbox.test.json", R"({"scene": "main.scene.json", "tests": {
        "save": {"steps": [{"command": "scene.save", "params": {"path": "saved.scene.json"}}]},
        "capture": {"steps": [{"command": "frame.capture",
                               "params": {"path": "frame.png", "scale": 0.25}}]},
        "record": {"steps": [{"command": "replay.record", "params": {"path": "run.replay.json"}}]},
        "storyboard": {"steps": [{"command": "storyboard.edit", "params": {"by": "test",
            "why": "trying", "edits": [{"id": "term/crate", "version": null,
                                        "set": {"name": "Crate", "summary": "A box."}}]}}]},
        "nested": {"steps": [{"command": "test.run"}]},
        "dry_run": {"steps": [{"command": "storyboard.edit", "params": {"dry_run": true,
            "edits": [{"id": "term/crate", "version": null, "set": {"name": "Crate",
                                                                    "summary": "A box."}}]}}]}}})");
    const json::Value result = project.run();
    std::vector<std::string> messages;
    for (const json::Value& item : result.find("failed")->as_array()) {
        messages.push_back(item.find("test")->as_string() + ": " +
                           item.find("message")->as_string());
    }
    CHECK(messages == std::vector<std::string>{
                          "capture: tests don't write files, so the project stays as it was",
                          "nested: a test can't run tests",
                          "record: tests don't write files, so a test can't record a replay",
                          "save: tests don't write files, so the project stays as it was",
                          "storyboard: tests don't write files, so the project stays as it was",
                      });
    CHECK(result.find("passed")->as_int() == 1);
    CHECK_FALSE(project.exists("saved.scene.json"));
    CHECK_FALSE(project.exists("frame.png"));
    CHECK_FALSE(project.exists("run.replay.json"));
    CHECK_FALSE(project.exists("storyboard"));
}

TEST_CASE("test.run takes files, folders and test names") {
    const Project project("test_run_paths");
    REQUIRE(slash::platform::create_directories(project.root + "/levels/tests").ok());
    REQUIRE(slash::platform::create_directories(project.root + "/build/tests").ok());
    const std::string passing =
        R"({"tests": {"NAME": {"steps": [{"command": "step", "expect": {"tick": 1}}]}}})";
    const auto test_named = [&](std::string_view name) {
        std::string text = passing;
        text.replace(text.find("NAME"), 4, name);
        return text;
    };
    project.write("tests/a.test.json", test_named("alpha"));
    project.write("levels/tests/b.test.json", test_named("beta"));
    project.write("build/tests/c.test.json", test_named("exported"));
    project.write("tests/broken.test.json", R"({"tests": {"empty": {"steps": []}}, "extra": 1})");
    project.write("tests/garbled.test.json", "{\"tests\": ");

    // The whole project skips build/; files that don't load are failed checks.
    const json::Value all = project.run();
    CHECK(all.find("tests")->as_int() == 2);
    CHECK(all.find("passed")->as_int() == 2);
    const json::Array& failed = all.find("failed")->as_array();
    REQUIRE(failed.size() == 3);
    CHECK(compact(failed[0]) == R"({"file":"tests/broken.test.json","message":)"
                                R"("unknown property \"extra\"","path":"/extra"})");
    CHECK(compact(failed[1]) ==
          R"({"file":"tests/broken.test.json","message":)"
          R"("expected at least 1 items, got 0","path":"/tests/empty/steps"})");
    CHECK(failed[2].find("file")->as_string() == "tests/garbled.test.json");
    CHECK(failed[2].find("message")->as_string().starts_with("not valid JSON: "));

    // Paths pick folders and files, each file once.
    CHECK(compact(project.run(R"({"paths": ["levels/", "levels/tests/b.test.json"]})")) ==
          R"({"failed":[],"passed":1,"tests":1})");
    // Files that don't load fail however the tests are picked.
    const json::Value named = project.run(R"({"paths": ["."], "tests": ["beta", "alpha"]})");
    CHECK(named.find("passed")->as_int() == 2);
    CHECK(named.find("failed")->size() == 3);

    const auto fails = [&](std::string_view params, int code, std::string_view message) {
        const command::Reply reply = project.call("test.run", params);
        REQUIRE_FALSE(reply.ok);
        CHECK(reply.code == code);
        CHECK(reply.message == message);
    };
    fails(R"({"tests": ["alhpa"]})", command::code::kNotFound,
          R"(/tests/0: no test is named "alhpa"; did you mean "alpha"?)");
    fails(R"({"paths": ["tests/a.tset.json"]})", command::code::kNotFound,
          R"(/paths/0: no folder or .test.json file "tests/a.tset.json" in the project)");
    fails(R"({"paths": ["tests/alpha.test.json"]})", command::code::kNotFound,
          R"(/paths/0: "tests/alpha.test.json": no such file; did you mean "tests/a.test.json"?)");
    fails(R"({"paths": ["../tests"]})", command::code::kPermissionDenied,
          R"(/paths/0: path "../tests" leaves the project folder)");
}

TEST_CASE("the whole project's test run checks the storyboard too") {
    const Project project("test_run_storyboard");
    project.write("main.scene.json", R"({"format": 1, "entities": {
        "hero": {"components": {"StoryRef": {"ids": ["character/mria"]}}}}})");
    REQUIRE(slash::platform::create_directories(project.root + "/storyboard").ok());
    project.write("storyboard/world.json", R"({"format": 1, "entries": {
        "character/mira": {"name": "Mira", "summary": "The hero."}}})");
    project.write("tests/a.test.json", R"({"tests": {"runs": {"steps": [{"command": "step"}]}}})");
    const json::Value all = project.run();
    REQUIRE(all.find("failed")->size() == 1);
    const json::Value& problem = all.find("failed")->as_array().front();
    CHECK(problem.find("file")->as_string() == "main.scene.json");
    CHECK(compact(problem) == R"({"file":"main.scene.json","message":)"
                              R"("unknown storyboard entry \"character/mria\"; )"
                              R"(did you mean \"character/mira\"?",)"
                              R"("path":"/entities/hero/components/StoryRef/ids/0"})");
    // Picking files or tests runs only those.
    CHECK(compact(project.run(R"({"paths": ["tests"]})")) ==
          R"({"failed":[],"passed":1,"tests":1})");
    CHECK(compact(project.run(R"({"tests": ["runs"]})")) ==
          R"({"failed":[],"passed":1,"tests":1})");
}

TEST_CASE("test files have a schema and validate") {
    CHECK(command::test_schema().find("title")->as_string() == "SlashEngine tests");
    CHECK(command::validate_test(parse(R"({"scene": "main.scene.json", "tests": {"a": {
        "steps": [{"command": "step", "params": {"ticks": 2}, "expect": {"tick": 2}}]}}})"))
              .empty());
    const auto errors = command::validate_test(parse(R"({"tests": {
        "a": {"steps": [{"params": {}}]}, "b": {"steps": [{"command": "step", "expect": 1,
                                                            "check": true}]}}})"));
    REQUIRE(errors.size() == 2);
    CHECK(errors[0].path == "/tests/a/steps/0");
    CHECK(errors[1].path == "/tests/b/steps/0/check");
    CHECK_FALSE(command::validate_test(parse(R"({"tests": {}})")).empty());
    CHECK_FALSE(command::validate_test(parse(R"({"scene": "", "tests": {"a": {"steps": [
        {"command": "step"}]}}})"))
                    .empty());
}
