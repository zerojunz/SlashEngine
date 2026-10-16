#pragma once

#include "slash/core/json.hpp"
#include "slash/core/result.hpp"

#include <string>
#include <utility>
#include <vector>

// Converts games made with other engines into SlashEngine projects. Only Godot 4 projects for
// now: their scenes, sprite frames, assets, input map and settings are converted, and each
// script becomes a storyboard task to port it to C++.
namespace slash::importer {

struct ImportOptions {
    // The other engine's project folder: for Godot, the folder holding project.godot.
    std::string source;
    // The SlashEngine project folder to write into. When it holds the source, the source's assets
    // stay where they are and are used from there; otherwise they are copied in under the paths
    // they had in the source.
    std::string destination;
};

// What an import writes, by path relative to the destination, and what it reports.
struct ImportPlan {
    std::vector<std::pair<std::string, std::string>> files;
    json::Value report;
};

// Reads the source project and plans its conversion, writing nothing. Fails only when the
// source isn't a project this can import; anything it can't convert is listed in the report's
// "unsupported" instead.
[[nodiscard]] Result<ImportPlan> plan_import(const ImportOptions& options);

// Plans the import and writes its files. Returns the report, with "files" set to how many files
// were written.
[[nodiscard]] Result<json::Value> import_project(const ImportOptions& options);

} // namespace slash::importer
