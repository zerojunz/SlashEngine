#include "slash/ai/provider.hpp"
#include "slash/bench/bench.hpp"
#include "slash/command/builtin.hpp"
#include "slash/command/command.hpp"
#include "slash/command/session.hpp"
#include "slash/core/json.hpp"
#include "slash/platform/file.hpp"
#include "slash/world/component_registry.hpp"
#include "slash/world/components.hpp"

#include <doctest/doctest.h>

#include <algorithm>
#include <cstddef>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace json = slash::json;
namespace bench = slash::bench;
namespace command = slash::command;
namespace ai = slash::ai;

namespace {

json::Value parse(std::string_view text) {
    auto parsed = json::parse(text);
    REQUIRE_MESSAGE(parsed.ok(), (parsed.ok() ? std::string() : parsed.error().message));
    return std::move(parsed).value();
}

// A folder of tasks, made fresh for a test.
struct TaskFolder {
    std::string path;

    explicit TaskFolder(std::string folder) : path(std::move(folder)) {
        std::filesystem::remove_all(path);
        std::filesystem::create_directories(path);
    }

    void add(const std::string& name, std::string_view task) const {
        write(path + "/" + name + "/task.json", task);
    }

    void add_file(const std::string& name, const std::string& file,
                  std::string_view contents) const {
        write(path + "/" + name + "/project/" + file, contents);
    }

    static void write(const std::string& file, std::string_view contents) {
        std::filesystem::create_directories(std::filesystem::path(file).parent_path());
        REQUIRE(slash::platform::write_file(file, contents).ok());
    }
};

bench::WithSession builtin_session() {
    return [](const std::string& root, const std::function<void(command::Session&)>& use) {
        slash::world::ComponentRegistry components;
        slash::world::register_builtin_components(components);
        command::Registry commands;
        command::register_builtin_commands(commands);
        command::SessionOptions options;
        options.project_root = root;
        options.capture_logs = false;
        command::Session session(components, commands, options);
        use(session);
    };
}

constexpr std::string_view kSpawn = R"({
  "format": 1,
  "prompt": "Add an entity called bat at (320, 120) to scenes/level.scene.json, and save.",
  "solution": [
    {"tool": "scene_open", "arguments": {"path": "scenes/level.scene.json"}},
    {"tool": "entity_create",
     "arguments": {"id": "bat", "components": {"Transform2D": {"position": [320, 120]}}}},
    {"tool": "scene_save"}
  ],
  "checks": [
    {"command": "scene.open", "params": {"path": "scenes/level.scene.json"}},
    {"command": "entity.get", "params": {"id": "bat"},
     "expect": {"components": {"Transform2D": {"position": [320, 120]}}}},
    {"step": 2, "expect": {"id": "bat"}}
  ]
})";

// A model that replies from a script, one reply per request, and keeps what it was asked.
class ScriptedModel final : public ai::Provider {
public:
    explicit ScriptedModel(std::vector<ai::Message> replies) : replies_(std::move(replies)) {}

    const ai::Capabilities& capabilities() const override { return capabilities_; }

    slash::Result<ai::Response> complete(const ai::Request& request, const ai::StreamSink& /*sink*/,
                                         const ai::CancelFlag* /*cancel*/) override {
        requests.push_back(request);
        if (next_ >= replies_.size()) {
            return slash::make_error(slash::ErrorCode::PlatformFailure, "the script ran out");
        }
        ai::Response response;
        response.message = replies_[next_++];
        response.message.role = ai::Role::Assistant;
        response.stop =
            response.message.tool_calls.empty() ? ai::StopReason::End : ai::StopReason::ToolCalls;
        response.usage = {.input_tokens = 1000, .output_tokens = 10};
        response.model = "scripted";
        return response;
    }

    std::vector<ai::Request> requests;

private:
    ai::Capabilities capabilities_;
    std::vector<ai::Message> replies_;
    std::size_t next_ = 0;
};

ai::ToolCall tool_call(std::string id, std::string name, std::string_view arguments) {
    return {.id = std::move(id), .name = std::move(name), .arguments = parse(arguments)};
}

constexpr std::string_view kLevel = R"({"entities": {"floor": {}}, "format": 1})";

} // namespace

TEST_CASE("estimate_tokens counts words, numbers, punctuation and lines as tokenizers do") {
    CHECK(bench::estimate_tokens("") == 0);
    CHECK(bench::estimate_tokens("hello") == 1);
    CHECK(bench::estimate_tokens("hello world") == 2);
    CHECK(bench::estimate_tokens("Transform2D") == 3); // "Transform", "2", "D"
    CHECK(bench::estimate_tokens("abcdefghijk") == 2); // eleven letters
    CHECK(bench::estimate_tokens("12345") == 2);       // digits go three at a time
    CHECK(bench::estimate_tokens(R"({"a":1})") == 5);  // {" a ": 1 }
    CHECK(bench::estimate_tokens("a\n    b") == 3);    // a break and its indent are one
    CHECK(bench::estimate_tokens("\xE6\x97\xA5\xE6\x9C\xAC") == 2); // two characters beyond ASCII
    CHECK(bench::estimate_tokens("    ") == 0);
}

TEST_CASE("validate_task checks steps and their checks") {
    CHECK(bench::validate_task(parse(kSpawn)).empty());

    auto errors = bench::validate_task(parse(R"({"format": 1, "prompt": "x", "solution": [],
                                                "extra": 1})"));
    CHECK_FALSE(errors.empty());

    errors = bench::validate_task(parse(R"({"format": 1, "prompt": "x",
        "solution": [{"tool": "scene_new"}],
        "checks": [{"step": 2, "expect": {}}]})"));
    REQUIRE(errors.size() == 1);
    CHECK(errors[0].path == "/checks/0/step");
    CHECK(errors[0].message == "the solution has only 1 steps");

    errors = bench::validate_task(parse(R"({"format": 1, "prompt": "x",
        "solution": [{"tool": "scene_new"}],
        "checks": [{"step": 1, "command": "entity.get", "expect": {}}, {"step": 1},
                   {"params": {}, "step": 1, "expect": {}}]})"));
    REQUIRE(errors.size() == 3);
    CHECK(errors[0].path == "/checks/0");
    CHECK(errors[1].message == "a step check needs expect");
    CHECK(errors[2].path == "/checks/2/params");
}

TEST_CASE("load_tasks reads a folder's tasks by name and suggests a misspelled one") {
    const TaskFolder folder("bench_load_tasks");
    folder.add("spawn", kSpawn);
    folder.add("another", kSpawn);
    std::filesystem::create_directories(folder.path + "/no-task-here");

    auto tasks = bench::load_tasks(folder.path);
    REQUIRE(tasks.ok());
    REQUIRE(tasks.value().size() == 2);
    CHECK(tasks.value()[0].name == "another");
    CHECK(tasks.value()[1].name == "spawn");

    const std::vector<std::string> names = {"spwan"};
    auto missing = bench::load_tasks(folder.path, names);
    REQUIRE_FALSE(missing.ok());
    CHECK(missing.error().message.find("did you mean \"spawn\"?") != std::string::npos);

    folder.add("broken", R"({"format": 1, "prompt": "x", "solution": [{"tool": 3}]})");
    auto broken = bench::load_tasks(folder.path);
    REQUIRE_FALSE(broken.ok());
    CHECK(broken.error().message.find("broken/task.json at /solution/0/tool") != std::string::npos);

    CHECK_FALSE(bench::load_tasks("bench_no_such_folder").ok());
}

TEST_CASE("run plays a task's solution through MCP, counts it and runs its checks") {
    const TaskFolder folder("bench_run_tasks");
    folder.add("spawn", kSpawn);
    folder.add_file("spawn", "scenes/level.scene.json", kLevel);

    auto tasks = bench::load_tasks(folder.path);
    REQUIRE(tasks.ok());
    bench::Options options;
    options.tasks = tasks.value();
    options.work = "bench_run_work";
    options.with_session = builtin_session();
    auto report = bench::run(options);
    REQUIRE(report.ok());
    const json::Value& value = report.value();
    CHECK(value.find("failed")->size() == 0);
    CHECK(value.find("setup")->find("tools")->as_int() > 0);
    CHECK(value.find("setup")->find("tool_definitions")->as_int() > 1000);
    const json::Value& spawn = *value.find("tasks")->find("spawn");
    CHECK(spawn.find("calls")->as_int() == 3);
    CHECK(spawn.find("written")->as_int() > 0);
    CHECK(spawn.find("read")->as_int() > 0);
    CHECK(spawn.find("tokens")->as_int() ==
          spawn.find("written")->as_int() + spawn.find("read")->as_int());
    CHECK(value.find("total")->find("tokens")->as_int() == spawn.find("tokens")->as_int());
    // The task's own folder is left as it was; the copy was saved.
    CHECK(slash::platform::read_file(folder.path + "/spawn/project/scenes/level.scene.json")
              .value() == kLevel);
    CHECK(slash::platform::read_file("bench_run_work/spawn/scenes/level.scene.json")
              .value()
              .find("bat") != std::string::npos);
}

TEST_CASE("run reports solution calls and checks that fail, with where") {
    const TaskFolder folder("bench_failing_tasks");
    folder.add("wrong", R"({
      "format": 1,
      "prompt": "Add bat.",
      "solution": [
        {"tool": "scene_new"},
        {"tool": "entity_create", "arguments": {"id": "bat"}},
        {"tool": "entity_destroy", "arguments": {"id": "rat"}}
      ],
      "checks": [
        {"command": "entity.get", "params": {"id": "bat"},
         "expect": {"components": {"Transform2D": {}}}},
        {"step": 2, "expect": {"id": "cat"}},
        {"command": "entity.get", "params": {"id": "rat"}}
      ]
    })");
    auto tasks = bench::load_tasks(folder.path);
    REQUIRE(tasks.ok());
    bench::Options options;
    options.tasks = tasks.value();
    options.work = "bench_failing_work";
    options.with_session = builtin_session();
    auto report = bench::run(options);
    REQUIRE(report.ok());
    const json::Array& failed = report.value().find("failed")->as_array();
    REQUIRE(failed.size() == 4);
    CHECK(failed[0].find("task")->as_string() == "wrong");
    CHECK(failed[0].find("path")->as_string() == "/solution/2");
    CHECK(failed[1].find("path")->as_string() == "/checks/0/expect/components/Transform2D");
    CHECK(failed[2].find("path")->as_string() == "/checks/1/expect/id");
    CHECK(failed[3].find("path")->as_string() == "/checks/2");
}

TEST_CASE("regressions finds what costs more than the baseline") {
    const json::Value baseline = parse(R"({
      "setup": {"instructions": 100, "tool_definitions": 1000, "tools": 10},
      "tasks": {"a": {"calls": 2, "tokens": 100}, "b": {"calls": 3, "tokens": 200}}
    })");
    json::Value report = parse(R"({
      "setup": {"instructions": 104, "tool_definitions": 1000, "tools": 12},
      "tasks": {"a": {"calls": 2, "tokens": 105}, "b": {"calls": 3, "tokens": 150}},
      "failed": []
    })");
    CHECK(bench::regressions(report, baseline, 0.05).empty());

    report = parse(R"({
      "setup": {"instructions": 100, "tool_definitions": 1100, "tools": 12},
      "tasks": {"a": {"calls": 3, "tokens": 100}, "b": {"calls": 3, "tokens": 211},
                "c": {"calls": 1, "tokens": 20}},
      "failed": []
    })");
    const json::Array found = bench::regressions(report, baseline, 0.05);
    REQUIRE(found.size() == 4);
    CHECK(found[0].find("what")->as_string() == "setup/tool_definitions");
    CHECK(found[0].find("baseline")->as_int() == 1000);
    CHECK(found[0].find("now")->as_int() == 1100);
    CHECK(found[1].find("what")->as_string() == "a/calls");
    CHECK(found[2].find("what")->as_string() == "b/tokens");
    CHECK(found[3].find("what")->as_string() == "c");
    CHECK(found[3].find("baseline")->is_null());

    CHECK(bench::baseline_of(report).find("failed") == nullptr);
}

TEST_CASE("run_live has a model do each task from its prompt with the MCP tools") {
    const TaskFolder folder("bench_live_tasks");
    folder.add("spawn", kSpawn);
    folder.add_file("spawn", "scenes/level.scene.json", kLevel);
    auto tasks = bench::load_tasks(folder.path);
    REQUIRE(tasks.ok());
    bench::Options options;
    options.tasks = tasks.value();
    options.work = "bench_live_work";
    options.with_session = builtin_session();

    auto model = std::make_shared<ScriptedModel>(std::vector<ai::Message>{
        {.tool_calls =
             {tool_call("1", "scene_open", R"({"path": "scenes/level.scene.json"})"),
              tool_call(
                  "2", "entity_create",
                  R"({"id": "bat", "components": {"Transform2D": {"position": [320, 120]}}})")}},
        {.tool_calls = {tool_call("3", "scene_save", "{}")}},
        {.text = "The bat is in the scene."}});
    bench::Live live;
    live.provider = model;
    auto report = bench::run_live(options, live);
    REQUIRE(report.ok());
    const json::Value& value = report.value();
    CHECK(value.find("failed")->size() == 0);
    CHECK(value.find("model")->as_string() == "scripted");
    const json::Value& spawn = *value.find("tasks")->find("spawn");
    CHECK(spawn.find("calls")->as_int() == 3);
    CHECK(spawn.find("turns")->as_int() == 3);
    CHECK(spawn.find("input_tokens")->as_int() == 3000);
    CHECK(spawn.find("output_tokens")->as_int() == 30);
    CHECK(spawn.find("tokens")->as_int() == 3030);

    // The model read what an agent over MCP reads: the instructions, the prompt and the tools,
    // then each tool's reply.
    REQUIRE(model->requests.size() == 3);
    const ai::Request& first = model->requests[0];
    REQUIRE(first.messages.size() == 2);
    CHECK(first.messages[0].role == ai::Role::System);
    CHECK_FALSE(first.messages[0].text.empty());
    CHECK(first.messages[1].text.find("bat") != std::string::npos);
    CHECK(std::ranges::any_of(first.tools, [](const ai::ToolSpec& tool) {
        return tool.name == "entity_create" && tool.parameters.is_object();
    }));
    const ai::Request& second = model->requests[1];
    REQUIRE(second.messages.size() == 5);
    CHECK(second.messages[3].role == ai::Role::Tool);
    CHECK(second.messages[3].tool_call_id == "1");
    CHECK(second.messages[4].text.find("bat") != std::string::npos);
    CHECK_FALSE(second.messages[4].is_error);
}

TEST_CASE("run_live fails a task the model leaves undone, wrong or unfinished") {
    const TaskFolder folder("bench_live_failing_tasks");
    folder.add("spawn", kSpawn);
    folder.add_file("spawn", "scenes/level.scene.json", kLevel);
    folder.add("count", R"({"format": 1, "prompt": "How many entities are there?",
                            "answer": "1", "solution": [{"tool": "world_query"}]})");
    folder.add_file("count", "scenes/level.scene.json", kLevel);
    auto tasks = bench::load_tasks(folder.path);
    REQUIRE(tasks.ok());
    bench::Options options;
    options.tasks = tasks.value();
    options.work = "bench_live_failing_work";
    options.with_session = builtin_session();

    // "count" (first by name): answers wrongly. "spawn": stops without saving, after a call
    // that fails.
    auto model = std::make_shared<ScriptedModel>(std::vector<ai::Message>{
        {.text = "There are 2."},
        {.tool_calls = {tool_call("1", "entity_destroy", R"({"id": "rat"})")}},
        {.text = "Done."}});
    bench::Live live;
    live.provider = model;
    auto report = bench::run_live(options, live);
    REQUIRE(report.ok());
    const json::Array& failed = report.value().find("failed")->as_array();
    REQUIRE(failed.size() == 2);
    CHECK(failed[0].find("task")->as_string() == "count");
    CHECK(failed[0].find("path")->as_string() == "/answer");
    CHECK(failed[1].find("task")->as_string() == "spawn");
    CHECK(failed[1].find("path")->as_string() == "/checks/1");
    REQUIRE(model->requests.size() == 3);
    CHECK(model->requests[2].messages.back().is_error);

    // A model that keeps calling tools runs out of turns.
    auto busy = std::make_shared<ScriptedModel>(
        std::vector<ai::Message>{{.tool_calls = {tool_call("1", "world_query", "{}")}},
                                 {.tool_calls = {tool_call("2", "world_query", "{}")}}});
    live.provider = busy;
    live.max_turns = 2;
    options.tasks = {tasks.value()[1]};
    report = bench::run_live(options, live);
    REQUIRE(report.ok());
    const json::Array& unfinished = report.value().find("failed")->as_array();
    REQUIRE(unfinished.size() == 1);
    CHECK(unfinished[0].find("message")->as_string() == "not done after 2 replies");
}
