#pragma once

#include "slash/command/command.hpp"
#include "slash/core/json.hpp"
#include "slash/core/json_schema.hpp"

#include <optional>
#include <string>
#include <string_view>
#include <vector>

// Headless tests: .test.json files of command calls and what their replies must hold, which
// test.run plays and answers with only the checks that failed. A file holds tests by name and
// the scene they start from:
//
//   {"scene": "main.scene.json",
//    "tests": {"crate_lands": {"steps": [
//        {"command": "run.until",
//         "params": {"where": [{"component": "RigidBody2D", "field": "sleeping",
//                               "value": true}]},
//         "expect": {"matched": true, "entities": ["crate"]}}]}}}
//
// Each test opens its scene file in a session of its own, so tests start alike and don't see
// one another or the session that runs them, then runs its steps in order. A step is a command
// call, as batch takes one, and optionally what its reply must hold: objects need only the
// members the step names, arrays the same number of items, and other values must be equal
// (numbers by value). A test stops at its first failed check: a command that fails, a reply that
// doesn't hold what the step expects, or a warning or an error logged while it ran. Tests can't
// write files, so they leave the project as it was.
namespace slash::command {

inline constexpr std::string_view kTestSuffix = ".test.json";

// Where a reply doesn't hold what a step expects, with path (which starts as the given one)
// pointing at the expected value: objects need only the expected members, arrays the same number
// of items, and other values must be equal, numbers by value.
struct Mismatch {
    std::string path;
    std::string message;
};
[[nodiscard]] std::optional<Mismatch>
mismatch(const json::Value& expected, const json::Value& actual, const std::string& path = "");

// The JSON Schema for .test.json files.
[[nodiscard]] json::Value test_schema();

// Every error in a test file. Commands and their params are checked when the test runs.
[[nodiscard]] std::vector<json::ValidationError> validate_test(const json::Value& test);

// Adds test.run.
void register_test_commands(Registry& registry);

} // namespace slash::command
