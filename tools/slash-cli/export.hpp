#pragma once

#include "slash/core/json.hpp"

#include <string>
#include <string_view>
#include <vector>

namespace slash::cli {

// The platforms slash-cli export knows, in the order usage text lists them.
inline const std::vector<std::string_view> kExportPlatforms{"macos", "windows", "linux", "web"};

struct ExportOptions {
    std::string platform;
    std::string project = ".";
    std::string output; // default: <project>/build/export/<platform>
    std::string engine; // engine folder with export/CMakeLists.txt; default: found automatically
    bool debug = false; // Debug build instead of Release
    bool test = false;  // run the exported game headless afterwards, where this machine can
};

struct ExportResult {
    bool ok = false;
    // On success: {"platform", "output", "executable", "bytes", "seconds", "log"} plus "test".
    // On failure: {"error": {"step", "message", ...}} with the validation errors or the end of
    // the build log, so the caller rarely needs to open the log.
    json::Value json;
};

// Exports a game project: validates project.json and every scene, stages them as the game's data,
// builds the engine and game for the platform with CMake, and lays the result out in the output
// folder. Progress goes to stderr as JSON lines and the full build output to a log file.
[[nodiscard]] ExportResult export_game(const ExportOptions& options);

} // namespace slash::cli
