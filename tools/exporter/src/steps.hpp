#pragma once

// Helpers shared by the exporter's sources. Not installed or used outside tools/exporter.

#include "slash/core/json.hpp"
#include "slash/platform/process.hpp"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace slash::exporter::detail {

// How many ticks an exported game runs in its headless test.
inline constexpr std::uint64_t kTestTicks = 60;

// What went wrong in an export. Each step fills one in and returns false.
struct Failure {
    std::string step;
    std::string message;
    json::Value details; // merged into the error object
};

// A path with '/' separators, for results and messages.
[[nodiscard]] std::string display(const std::filesystem::path& path);

// The absolute, normalized form of path.
[[nodiscard]] std::string absolute_text(const std::filesystem::path& path);

// The value of an environment variable, or "" if it is not set.
[[nodiscard]] std::string environment(std::string_view name);

// Whether `program --version` runs and succeeds.
[[nodiscard]] bool program_runs(const std::string& program);

// A program on PATH (with ".exe" on Windows), or "" if there is none.
[[nodiscard]] std::string find_on_path(std::string_view name);

// Runs one build step, appending its output to the log. On failure, records the step with the
// end of its output.
bool step(const std::string& name, const std::vector<std::string>& args,
          const platform::ProcessOptions& process, Failure& failure);

// Signs each library and framework in a bundle's Frameworks folder with args (codesign and its
// options) followed by its path, as Apple wants nested code signed before the bundle around it.
// Does nothing if there is no such folder.
bool sign_nested(const std::filesystem::path& frameworks, const std::vector<std::string>& args,
                 const platform::ProcessOptions& process, Failure& failure);

// The finished game's summary line in the output of `<game> --headless --ticks n`.
[[nodiscard]] std::optional<json::Value> find_summary(const std::string& output);

// Copies what an export's test reports from the game's summary into test: its ticks, entities
// and the services' backends, if it ran on any.
void add_summary(json::Value& test, const json::Value& summary);

// The total size of the files in a folder.
[[nodiscard]] std::uint64_t folder_bytes(const std::filesystem::path& folder);

} // namespace slash::exporter::detail
