#include "slash/importer/import.hpp"

#include "godot_import.hpp"
#include "unity_import.hpp"

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
    const bool godot = fs::exists(source / "project.godot", error);
    const bool unity = !godot && fs::exists(source / "ProjectSettings/ProjectVersion.txt", error) &&
                       fs::is_directory(source / "Assets", error);
    if (!godot && !unity) {
        return Error{.code = ErrorCode::InvalidArgument,
                     .message = "no Godot or Unity project in " + options.source +
                                ": a Godot 4 project has project.godot, a Unity project Assets/ "
                                "and ProjectSettings/ProjectVersion.txt (Unreal is on the "
                                "roadmap)"};
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
    auto existing_project = read_optional(destination / runtime::kProjectFile);
    auto existing_work = read_optional(destination / storyboard::file_path("work"));
    if (unity) {
        return plan_unity(source.generic_string() + "/", prefix, !contained, existing_project,
                          existing_work);
    }
    return plan_godot(source.generic_string() + "/", prefix, !contained, existing_project,
                      existing_work);
}

Result<ImportPlan> import_project(const ImportOptions& options) {
    auto plan = plan_import(options);
    if (!plan) {
        return plan;
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
    plan.value().report["files"] = plan.value().files.size();
    return plan;
}

} // namespace slash::importer
