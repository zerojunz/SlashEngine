#include "slash/importer/import.hpp"

#include "common.hpp"
#include "godot_import.hpp"
#include "unity_import.hpp"

#include "slash/importer/godot_pack.hpp"
#include "slash/importer/unity_build.hpp"
#include "slash/platform/file.hpp"
#include "slash/runtime/project.hpp"
#include "slash/storyboard/storyboard.hpp"

#include <algorithm>
#include <filesystem>
#include <iterator>
#include <optional>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

namespace slash::importer {
namespace {

namespace fs = std::filesystem;

// A file's text, or nullopt if it can't be read.
std::optional<std::string> read_optional(const fs::path& path) {
    auto text = platform::read_file(path.string());
    return text ? std::optional<std::string>(std::move(text.value())) : std::nullopt;
}

bool is_code_file(std::string_view path) {
    return path.ends_with(".cpp") || path.ends_with(".hpp") || path.ends_with(".h");
}

// What the destination has that an import builds on: its project.json, storyboard/work.json and
// the C++ files of its gameplay sources and of the folder skeletons go to.
Existing existing_of(const fs::path& destination) {
    Existing existing;
    existing.project = read_optional(destination / runtime::kProjectFile);
    existing.work = read_optional(destination / storyboard::file_path("work"));
    std::vector<std::string> sources{gameplay_folder(existing.project)};
    if (auto parsed = json::parse(existing.project.value_or("")); parsed) {
        if (auto project = runtime::parse_project(parsed.value()); project) {
            std::ranges::copy(project.value().gameplay_sources, std::back_inserter(sources));
        }
    }
    for (const std::string& source : sources) {
        const fs::path path = destination / source;
        std::error_code error;
        if (fs::is_regular_file(path, error)) {
            if (auto text = read_optional(path); text && is_code_file(source)) {
                existing.code[fs::path(source).lexically_normal().generic_string()] = *text;
            }
            continue;
        }
        auto files = platform::list_files(path.string());
        if (!files) {
            continue;
        }
        for (const std::string& file : files.value()) {
            const fs::path relative = (fs::path(source) / file).lexically_normal();
            if (auto text = read_optional(path / file); text && is_code_file(file)) {
                existing.code[relative.generic_string()] = std::move(*text);
            }
        }
    }
    return existing;
}

// Where unpack_to puts an unpacked project's files: "" or a folder in the destination ending in
// '/'.
Result<std::string> unpack_prefix(const fs::path& destination, const ImportOptions& options) {
    if (options.unpack_to.empty()) {
        return std::string();
    }
    std::error_code error;
    const fs::path folder = fs::weakly_canonical(destination / options.unpack_to, error);
    const std::string inside = folder.lexically_relative(destination).generic_string();
    if (error || inside.empty() || inside.starts_with("..")) {
        return Error{.code = ErrorCode::InvalidArgument,
                     .message = "unpack_to " + options.unpack_to +
                                " leaves the project: name a folder in it"};
    }
    return inside == "." ? std::string() : inside + "/";
}

// Unpacks a game exported with Godot, then plans importing the project that comes out of it.
Result<ImportPlan> plan_unpacked(const fs::path& source, const fs::path& destination,
                                 const ImportOptions& options) {
    auto bytes = platform::read_file(source.string());
    if (!bytes) {
        return bytes.error();
    }
    auto game = godot::unpack_game(bytes.value());
    if (!game) {
        return game.error();
    }
    auto unpack = unpack_prefix(destination, options);
    if (!unpack) {
        return unpack.error();
    }
    const std::string& prefix = unpack.value();
    auto plan = plan_godot(source.generic_string(), &game.value().files, prefix,
                           options.unpack_to.empty(), existing_of(destination));
    if (!plan) {
        return plan;
    }
    if (!options.unpack_to.empty()) {
        std::vector<std::pair<std::string, std::string>> files;
        files.reserve(game.value().files.size() + plan.value().files.size());
        for (auto& [path, contents] : game.value().files) {
            files.emplace_back(prefix + path, std::move(contents));
        }
        std::ranges::move(plan.value().files, std::back_inserter(files));
        plan.value().files = std::move(files);
        game.value().report["folder"] = prefix.empty() ? "." : prefix.substr(0, prefix.size() - 1);
    }
    plan.value().report["unpacked"] = std::move(game.value().report);
    return plan;
}

// Rebuilds a Unity build's project with AssetRipper in the destination's .slash/import/unity/,
// then plans importing it.
Result<ImportPlan> plan_unity_build(const unity::Build& build, const fs::path& destination,
                                    const ImportOptions& options) {
    auto unpack = unpack_prefix(destination, options);
    if (!unpack) {
        return unpack.error();
    }
    const std::string& prefix = unpack.value();
    const fs::path work = destination / ".slash" / "import" / "unity";
    std::error_code error;
    fs::remove_all(work, error);
    if (auto made = platform::create_directories(work.string()); !made) {
        return made.error();
    }
    auto unpacked = unity::unpack_build(
        build, {.assetripper = options.assetripper, .work = work, .start_timeout = 120});
    if (!unpacked) {
        return unpacked.error();
    }
    const std::string project = unpacked.value().project.generic_string() + "/";
    auto plan = plan_unity(project, prefix, options.unpack_to.empty(), existing_of(destination));
    if (!plan) {
        return plan;
    }
    json::Value& report = unpacked.value().report;
    if (!options.unpack_to.empty()) {
        // The whole Unity project comes along, as AssetRipper wrote it.
        auto files = platform::list_files(project);
        if (!files) {
            return files.error();
        }
        std::vector<std::pair<std::string, std::string>> all;
        all.reserve(files.value().size() + plan.value().files.size());
        for (const std::string& file : files.value()) {
            auto bytes = platform::read_file(project + file);
            if (!bytes) {
                return bytes.error();
            }
            all.emplace_back(prefix + file, std::move(bytes.value()));
        }
        std::ranges::move(plan.value().files, std::back_inserter(all));
        plan.value().files = std::move(all);
        report["folder"] = prefix.empty() ? "." : prefix.substr(0, prefix.size() - 1);
    }
    report["log"] = (work / "assetripper.log").lexically_relative(destination).generic_string();
    plan.value().report["unpacked"] = std::move(report);
    return plan;
}

} // namespace

Result<ImportPlan> plan_import(const ImportOptions& options) {
    std::error_code error;
    const fs::path source = fs::weakly_canonical(fs::absolute(options.source, error), error);
    const fs::path destination =
        fs::weakly_canonical(fs::absolute(options.destination, error), error);
    if (!error && fs::is_regular_file(source, error)) {
        if (auto build = unity::find_build(source)) {
            return plan_unity_build(*build, destination, options);
        }
        return plan_unpacked(source, destination, options);
    }
    if (error || !fs::is_directory(source, error)) {
        return Error{.code = ErrorCode::NotFound,
                     .message = "no folder or file at " + options.source};
    }
    const bool godot = fs::exists(source / "project.godot", error);
    const bool unity = !godot && fs::exists(source / "ProjectSettings/ProjectVersion.txt", error) &&
                       fs::is_directory(source / "Assets", error);
    if (!godot && !unity) {
        if (auto build = unity::find_build(source)) {
            return plan_unity_build(*build, destination, options);
        }
        return Error{.code = ErrorCode::InvalidArgument,
                     .message = "no Godot or Unity project or Unity build in " + options.source +
                                ": a Godot 4 project has project.godot, a Unity project Assets/ "
                                "and ProjectSettings/ProjectVersion.txt, a Unity build a "
                                "Game_Data folder (or Build/*.data on the web) (Unreal is on the "
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
    const Existing existing = existing_of(destination);
    if (unity) {
        return plan_unity(source.generic_string() + "/", prefix, !contained, existing);
    }
    return plan_godot(source.generic_string() + "/", nullptr, prefix, !contained, existing);
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
