#pragma once

#include "slash/ai/provider.hpp"
#include "slash/command/session.hpp"
#include "slash/core/json.hpp"
#include "slash/core/json_schema.hpp"
#include "slash/core/result.hpp"

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

// A benchmark of standard agent tasks, counting the calls and tokens each one costs an agent over
// MCP. A task is a folder with a task.json and the project it starts from (project/):
//
//   {"format": 1,
//    "prompt": "Move every rigid body in scenes/level.scene.json 32 pixels up, and save.",
//    "solution": [{"tool": "scene_open", "arguments": {"path": "scenes/level.scene.json"}}, ...],
//    "checks": [{"command": "entity.get", "params": {"id": "crate_1"},
//                "expect": {"components": {"Transform2D": {"position": [100, 168]}}}},
//               {"step": 2, "expect": {"count": 4}}]}
//
// The solution is the calls an agent that knows the engine well makes, as MCP tool calls. The
// benchmark plays it through the MCP server in a fresh copy of the project and counts what the
// model would write and read; then it runs the checks, .test.json steps or what a solution step
// (from 1) replied, so a task that no longer does what it says fails. Measured against a
// baseline, it catches commands that answer at greater length or tool definitions that grow.
namespace slash::bench {

// Roughly how many tokens a model's tokenizer makes of the text, the same on every machine.
// Tokenizers differ by model, so this compares runs with each other: on the engine's JSON and
// English it comes within a tenth of a real tokenizer's count.
[[nodiscard]] std::int64_t estimate_tokens(std::string_view text);

// The JSON Schema for task.json files.
[[nodiscard]] json::Value task_schema();
[[nodiscard]] std::vector<json::ValidationError> validate_task(const json::Value& task);

struct Task {
    std::string name;   // the folder's name
    std::string folder; // where it is
    json::Value spec;   // its task.json
};

// The tasks in a folder: each folder in it with a task.json, by name. With names, only those.
[[nodiscard]] Result<std::vector<Task>> load_tasks(const std::string& folder,
                                                   std::span<const std::string> names = {});

// Makes a session over a project folder and calls use with it. The caller registers the engine's
// components and commands, as slash-mcp does, so the benchmark sees the tools agents see.
using WithSession = std::function<void(const std::string& project_root,
                                       const std::function<void(command::Session&)>& use)>;

struct Options {
    std::vector<Task> tasks;
    // Where each task's project is copied before it runs, as <work>/<task>.
    std::string work;
    WithSession with_session;
};

// Runs every task and reports what each cost:
//
//   {"setup": {"instructions": 2897, "tools": 85, "tool_definitions": 19145},
//    "tasks": {"raise-crates": {"calls": 4, "written": 160, "read": 230, "tokens": 390}},
//    "total": {"calls": 4, "tokens": 390}, "failed": []}
//
// "setup" is what an agent reads once per session: the server's instructions and the tool
// definitions, in estimated tokens. For each task, "written" is the tool calls (names and
// arguments) and "read" what came back; "failed" lists a task's failed solution calls and checks.
[[nodiscard]] Result<json::Value> run(const Options& options);

// A live run: a model does each task from its prompt alone, with the MCP server's instructions as
// its system message and its tools, as an agent connected to slash-mcp would.
struct Live {
    std::shared_ptr<ai::Provider> provider;
    int max_turns = 20;                     // replies the model may take per task
    std::optional<std::int64_t> max_tokens; // per reply, where the provider's config sets none
};

// Runs every task live and reports what each cost as the provider counted it:
//
//   {"model": "...", "tasks": {"raise-crates": {"calls": 4, "turns": 3, "input_tokens": 61234,
//    "output_tokens": 412, "tokens": 61646}}, "total": {"calls": 4, "tokens": 61646},
//    "failed": []}
//
// A task fails when the model gives up, runs out of turns, a check fails or its final reply
// lacks the task's "answer". Checks on solution steps are skipped: the model's steps are its own.
[[nodiscard]] Result<json::Value> run_live(const Options& options, const Live& live);

// Where a report costs more than the baseline: setup or a task's tokens more than tolerance
// (a fraction) above it, a task taking more calls, or a task the baseline lacks. Each item is
// {"what", "baseline", "now"}; what is "setup/tool_definitions", "raise-crates/tokens" and so on.
[[nodiscard]] json::Array regressions(const json::Value& report, const json::Value& baseline,
                                      double tolerance);

// What a report keeps in a baseline file: the numbers, not the failures.
[[nodiscard]] json::Value baseline_of(const json::Value& report);

} // namespace slash::bench
