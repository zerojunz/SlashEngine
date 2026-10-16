#include "slash/importer/import.hpp"

#include "godot_import.hpp"

#include "slash/platform/file.hpp"
#include "slash/runtime/project.hpp"
#include "slash/storyboard/storyboard.hpp"

#include <filesystem>
#include <optional>
#include <string>
#include <system_error>

namespace slash::importer {
namespace {

namespace fs = std::filesystem;

// A file's text, or nullopt if it can't be read.
std::optional<std::string> read_optional(const fs::path& path) {
    auto text = platform::read_file(path.string());
    return text ? std::optional<std::string>(std::move(text.value())) : std::nullopt;
}

} // namespace

Result<ImportPlan> plan_import(const ImportOptions& options) {
    std::error_code error;
    const fs::path source = fs::weakly_canonical(fs::absolute(options.source, error), error);
    const fs::path destination =
        fs::weakly_canonical(fs::absolute(options.destination, error), error);
    if (error || !fs::is_directory(source, error)) {
        return Error{.code = ErrorCode::NotFound, .message = "no folder at " + options.source};
    }
    if (!fs::exists(source / "project.godot", error)) {
        return Error{.code = ErrorCode::InvalidArgument,
                     .message = "no project.godot in " + options.source +
                                ": only Godot 4 projects can be imported for now (Unity and "
                                "Unreal are on the roadmap)"};
    }
    // A source inside the destination stays where it is and is used from there; any other is
    // copied in.
    const fs::path relative = source.lexically_relative(destination);
    const std::string inside = relative.generic_string();
    const bool contained = !relative.empty() && !inside.starts_with("..");
    std::string prefix;
    if (contained && inside != ".") {
        prefix = inside + "/";
    }
    return plan_godot(source.generic_string() + "/", prefix, !contained,
                      read_optional(destination / runtime::kProjectFile),
                      read_optional(destination / storyboard::file_path("work")));
}

Result<json::Value> import_project(const ImportOptions& options) {
    auto plan = plan_import(options);
    if (!plan) {
        return plan.error();
    }
    const fs::path destination(options.destination);
    for (const auto& [path, contents] : plan.value().files) {
        const fs::path full = destination / path;
        if (auto made = platform::create_directories(full.parent_path().string()); !made) {
            return made.error();
        }
        if (auto written = platform::write_file(full.string(), contents); !written) {
            return written.error();
        }
    }
    json::Value report = std::move(plan.value().report);
    report["files"] = plan.value().files.size();
    return report;
}

} // namespace slash::importer
