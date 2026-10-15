#pragma once

#include "slash/core/json.hpp"

#include <optional>
#include <string>

namespace slash::exporter {

struct ExportOptions {
    std::string target;        // preset name from export_presets.json, or a platform
    std::string project = "."; // game folder with project.json
    std::string output;        // default: the preset's output, else build/export/<preset>
    std::string engine; // engine folder with export/CMakeLists.txt; default: found automatically
    bool debug = false; // Debug build even if the preset says Release
    bool test = false;  // run the exported game headless afterwards, where this machine can
};

struct ExportResult {
    bool ok = false;
    // On success: {"preset", "platform", "output", "executable", "bytes", "seconds", "log"} plus
    // "signed" and "test" when they apply. On failure: {"error": {"step", "message", ...}} with
    // the validation errors or the end of the build log, so the caller rarely needs to open the
    // log. A step of "options" means the request itself was wrong.
    json::Value json;
};

// Exports a game project: validates project.json, export_presets.json and every scene, prefab,
// texture (.png, .bmp), .frames.json and .material.json file, stages them as the game's data,
// builds the engine and game for the platform with CMake, signs the result and lays it out in
// the output folder. Progress goes to the log as JSON lines and the full build output to a log
// file.
[[nodiscard]] ExportResult export_game(const ExportOptions& options);

// The engine folder exports build from: engine if given and valid, else SLASH_ENGINE_DIR, an
// installed copy next to the running program, or the checkout the tools were built from.
[[nodiscard]] std::optional<std::string> find_engine(const std::string& engine = {});

} // namespace slash::exporter
