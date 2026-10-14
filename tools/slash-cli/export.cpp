#include "export.hpp"

#include "slash/core/log.hpp"
#include "slash/core/text.hpp"
#include "slash/platform/file.hpp"
#include "slash/platform/process.hpp"
#include "slash/runtime/project.hpp"
#include "slash/world/component_registry.hpp"
#include "slash/world/components.hpp"
#include "slash/world/scene.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#ifndef SLASH_ENGINE_SOURCE_DIR
#define SLASH_ENGINE_SOURCE_DIR ""
#endif

namespace slash::cli {
namespace {

namespace fs = std::filesystem;

constexpr std::string_view kMacosDeploymentTarget = "11.0";
constexpr std::string_view kSceneSuffix = ".scene.json";
constexpr std::uint64_t kTestTicks = 60;

// What went wrong in an export. Each step fills one in and returns false.
struct Failure {
    std::string step;
    std::string message;
    json::Value details; // merged into the error object
};

std::string display(const fs::path& path) {
    return path.lexically_normal().generic_string();
}

std::string environment(const char* name) {
    return platform::environment_variable(name);
}

bool host_builds(std::string_view platform) {
#if defined(__APPLE__)
    return platform == "macos" || platform == "web";
#elif defined(_WIN32)
    return platform == "windows" || platform == "web";
#elif defined(__linux__)
    return platform == "linux" || platform == "web";
#else
    return platform == "web";
#endif
}

std::string host_name() {
#if defined(__APPLE__)
    return "macos";
#elif defined(_WIN32)
    return "windows";
#elif defined(__linux__)
    return "linux";
#else
    return "this system";
#endif
}

bool is_engine_dir(const fs::path& path) {
    std::error_code error;
    return !path.empty() && fs::exists(path / "export" / "CMakeLists.txt", error);
}

std::optional<fs::path> find_engine(const ExportOptions& options) {
    if (!options.engine.empty()) {
        return is_engine_dir(options.engine) ? std::optional<fs::path>(options.engine)
                                             : std::nullopt;
    }
    for (const fs::path& candidate :
         {fs::path(environment("SLASH_ENGINE_DIR")),
          fs::path(platform::base_path()) / ".." / "share" / "slashengine",
          fs::path(SLASH_ENGINE_SOURCE_DIR)}) {
        if (is_engine_dir(candidate)) {
            return fs::absolute(candidate).lexically_normal();
        }
    }
    return std::nullopt;
}

// The Emscripten CMake toolchain file from an activated emsdk.
std::optional<fs::path> find_emscripten_toolchain() {
    const fs::path relative = fs::path("cmake") / "Modules" / "Platform" / "Emscripten.cmake";
    std::vector<fs::path> roots;
    if (const std::string emscripten = environment("EMSCRIPTEN"); !emscripten.empty()) {
        roots.emplace_back(emscripten);
    }
    if (const std::string emsdk = environment("EMSDK"); !emsdk.empty()) {
        roots.push_back(fs::path(emsdk) / "upstream" / "emscripten");
    }
    std::error_code error;
    for (const fs::path& root : roots) {
        if (fs::exists(root / relative, error)) {
            return root / relative;
        }
    }
    return std::nullopt;
}

bool program_runs(const std::string& program) {
    auto result = platform::run_process({program, "--version"});
    return result && result.value().exit_code == 0;
}

// Skips folders an export must never ship: hidden ones (.git, .slash), build output and game
// sources.
bool skipped_folder(const fs::path& name) {
    const std::string text = name.string();
    return text.starts_with(".") || text == "build" || text == "game";
}

// Copies project.json and every scene into data, validating each scene on the way.
bool stage_data(const fs::path& project_dir, const runtime::Project& project, const fs::path& data,
                Failure& failure) {
    std::error_code error;
    fs::remove_all(data, error);
    fs::create_directories(data, error);
    if (error) {
        failure = {"stage", "could not create " + display(data) + ": " + error.message(), {}};
        return false;
    }
    fs::copy_file(project_dir / runtime::kProjectFile, data / runtime::kProjectFile, error);
    if (error) {
        failure = {"stage", "could not copy project.json: " + error.message(), {}};
        return false;
    }

    world::ComponentRegistry components;
    world::register_builtin_components(components);
    json::Array problems;
    bool found_main = false;
    auto it = fs::recursive_directory_iterator(project_dir, error);
    for (; !error && it != fs::recursive_directory_iterator(); it.increment(error)) {
        const fs::directory_entry& entry = *it;
        if (entry.is_directory(error)) {
            if (skipped_folder(entry.path().filename())) {
                it.disable_recursion_pending();
            }
            continue;
        }
        const std::string name = entry.path().filename().string();
        if (!name.ends_with(kSceneSuffix)) {
            continue;
        }
        const fs::path relative = entry.path().lexically_relative(project_dir);
        const std::string file = relative.generic_string();
        found_main = found_main || file == project.main_scene;
        auto text = platform::read_file(entry.path().string());
        std::vector<json::ValidationError> errors;
        if (!text) {
            errors.push_back({"", text.error().message});
        } else if (auto parsed = json::parse(text.value()); !parsed) {
            errors.push_back({"", parsed.error().message});
        } else {
            errors = world::validate_scene(components, parsed.value());
        }
        for (const json::ValidationError& problem : errors) {
            json::Value item;
            item["file"] = file;
            item["path"] = problem.path;
            item["message"] = problem.message;
            problems.push_back(std::move(item));
        }
        fs::create_directories((data / relative).parent_path(), error);
        fs::copy_file(entry.path(), data / relative, fs::copy_options::overwrite_existing, error);
        if (error) {
            failure = {"stage", "could not copy " + file + ": " + error.message(), {}};
            return false;
        }
    }
    if (error) {
        failure = {"stage", "could not list the project folder: " + error.message(), {}};
        return false;
    }
    if (!found_main) {
        json::Value item;
        item["file"] = std::string(runtime::kProjectFile);
        item["path"] = "/main_scene";
        item["message"] = "the main scene \"" + project.main_scene + "\" does not exist";
        problems.push_back(std::move(item));
    }
    if (!problems.empty()) {
        failure.step = "validate";
        failure.message = std::to_string(problems.size()) + " problem" +
                          (problems.size() == 1 ? "" : "s") + " in the project";
        failure.details["errors"] = std::move(problems);
        return false;
    }
    return true;
}

std::uint64_t folder_bytes(const fs::path& folder) {
    std::uint64_t total = 0;
    std::error_code error;
    for (auto it = fs::recursive_directory_iterator(folder, error);
         !error && it != fs::recursive_directory_iterator(); it.increment(error)) {
        if (it->is_regular_file(error)) {
            total += it->file_size(error);
        }
    }
    return total;
}

// Runs one build step, appending its output to the log. On failure, records the step with the
// end of its output.
bool step(const std::string& name, const std::vector<std::string>& args,
          const platform::ProcessOptions& process, Failure& failure) {
    log::info("export step", {{"step", name}});
    auto result = platform::run_process(args, process);
    if (!result) {
        failure = {name, result.error().message, {}};
        return false;
    }
    if (result.value().exit_code != 0) {
        failure.step = name;
        failure.message = args[0] + " failed with exit code " +
                          std::to_string(result.value().exit_code) + " during " + name;
        failure.details["tail"] = result.value().tail;
        return false;
    }
    return true;
}

std::string absolute_text(const fs::path& path) {
    std::error_code error;
    return fs::absolute(path, error).lexically_normal().generic_string();
}

// An existing output folder is replaced only if a previous export made it, so a mistyped --out
// can't delete someone's files. Checked before building, so the refusal comes right away.
bool output_writable(const fs::path& output, const fs::path& record, Failure& failure) {
    std::error_code error;
    if (!fs::exists(output, error) || fs::is_empty(output, error)) {
        return true;
    }
    if (auto previous = platform::read_file(record.string()); previous) {
        if (auto parsed = json::parse(previous.value()); parsed) {
            const json::Value* recorded = parsed.value().find("output");
            if (recorded != nullptr && recorded->is_string() &&
                recorded->as_string() == absolute_text(output)) {
                return true;
            }
        }
    }
    failure = {"options",
               display(output) + " already exists and was not made by slash-cli export; delete "
                                 "it or pass another --out",
               {}};
    return false;
}

// Replaces the output folder with the freshly installed package.
bool publish(const fs::path& package, const fs::path& output, const fs::path& record,
             Failure& failure) {
    std::error_code error;
    fs::remove_all(output, error);
    fs::create_directories(output.parent_path(), error);
    fs::rename(package, output, error);
    if (error) {
        error.clear();
        fs::copy(package, output, fs::copy_options::recursive, error);
    }
    if (error) {
        failure = {"publish", "could not write " + display(output) + ": " + error.message(), {}};
        return false;
    }
    json::Value written;
    written["output"] = absolute_text(output);
    (void)platform::write_file(record.string(), json::write(written));
    return true;
}

// The finished game's summary line in the output of `<game> --headless --ticks n`.
std::optional<json::Value> find_summary(const std::string& output) {
    std::size_t end = output.size();
    while (end > 0) {
        std::size_t start = output.rfind('\n', end - 1);
        start = start == std::string::npos ? 0 : start + 1;
        auto parsed = json::parse(std::string_view(output).substr(start, end - start));
        if (parsed && parsed.value().is_object() && parsed.value().find("ticks") != nullptr &&
            parsed.value().find("scene") != nullptr) {
            return std::move(parsed).value();
        }
        end = start == 0 ? 0 : start - 1;
    }
    return std::nullopt;
}

json::Value run_test(const ExportOptions& options, const fs::path& output, const fs::path& program,
                     const fs::path& log_path) {
    json::Value test;
    std::vector<std::string> args;
    if (options.platform == "web") {
        if (!program_runs("node")) {
            test["skipped"] = "Node.js is not on PATH";
            return test;
        }
        args = {"node", program.string()};
    } else if (options.platform == host_name()) {
        args = {program.string()};
    } else {
        test["skipped"] = "this machine can't run " + options.platform + " games";
        return test;
    }
    args.insert(args.end(), {"--headless", "--ticks", std::to_string(kTestTicks)});
    log::info("export step", {{"step", "test"}});
    platform::ProcessOptions process;
    process.working_directory = output.string();
    process.log_path = log_path.string();
    process.tail_lines = 20;
    auto result = platform::run_process(args, process);
    if (!result) {
        test["passed"] = false;
        test["message"] = result.error().message;
        return test;
    }
    const auto summary = find_summary(result.value().tail);
    const bool passed = result.value().exit_code == 0 && summary.has_value() &&
                        summary->find("ticks")->is_int() &&
                        summary->find("ticks")->as_int() == static_cast<std::int64_t>(kTestTicks);
    test["passed"] = passed;
    if (summary.has_value()) {
        test["ticks"] = *summary->find("ticks");
        if (const json::Value* entities = summary->find("entities"); entities != nullptr) {
            test["entities"] = *entities;
        }
    }
    if (!passed) {
        test["exit_code"] = result.value().exit_code;
        test["tail"] = result.value().tail;
    }
    return test;
}

ExportResult fail(const Failure& failure, const fs::path& log_path) {
    json::Value error = failure.details.is_object() ? failure.details : json::Value(json::Object{});
    error["step"] = failure.step;
    error["message"] = failure.message;
    std::error_code ignored;
    if (!log_path.empty() && fs::exists(log_path, ignored)) {
        error["log"] = display(log_path);
    }
    ExportResult result;
    result.json["error"] = std::move(error);
    return result;
}

} // namespace

ExportResult export_game(const ExportOptions& options) {
    const auto started = std::chrono::steady_clock::now();
    const fs::path project_dir = options.project;
    const fs::path work = project_dir / ".slash" / "export" / options.platform;
    const fs::path output = options.output.empty()
                                ? project_dir / "build" / "export" / options.platform
                                : fs::path(options.output);
    const fs::path log_path = work / "export.log";

    if (std::ranges::find(kExportPlatforms, options.platform) == kExportPlatforms.end()) {
        std::string message = "unknown platform \"" + options.platform + "\"";
        if (const std::string_view hint = closest_match(options.platform, kExportPlatforms);
            !hint.empty()) {
            message += "; did you mean \"" + std::string(hint) + "\"?";
        }
        return fail({"options", message + " (ios and android are not supported yet)", {}}, {});
    }
    if (!host_builds(options.platform)) {
        return fail({"options",
                     options.platform + " games can't be built on " + host_name() +
                         " yet; build them on " + options.platform + " or in CI",
                     {}},
                    {});
    }

    auto project = runtime::load_project(project_dir.string());
    if (!project) {
        json::Value details;
        if (auto text = platform::read_file((project_dir / runtime::kProjectFile).string()); text) {
            if (auto parsed = json::parse(text.value()); parsed) {
                json::Array errors;
                for (const auto& problem : runtime::validate_project(parsed.value())) {
                    json::Value item;
                    item["file"] = std::string(runtime::kProjectFile);
                    item["path"] = problem.path;
                    item["message"] = problem.message;
                    errors.push_back(std::move(item));
                }
                if (!errors.empty()) {
                    details["errors"] = std::move(errors);
                }
            }
        }
        return fail({"validate", project.error().message, std::move(details)}, {});
    }

    const std::optional<fs::path> engine = find_engine(options);
    if (!engine) {
        return fail({"options",
                     "could not find the engine sources; pass --engine <folder> or set "
                     "SLASH_ENGINE_DIR to a SlashEngine checkout",
                     {}},
                    {});
    }
    std::optional<fs::path> toolchain;
    if (options.platform == "web") {
        toolchain = find_emscripten_toolchain();
        if (!toolchain) {
            return fail({"options",
                         "web exports need Emscripten; install emsdk and activate it (source "
                         "emsdk_env.sh) so EMSDK is set",
                         {}},
                        {});
        }
    }
    if (!program_runs("cmake")) {
        return fail({"options", "exports need CMake 3.25 or newer on PATH", {}}, {});
    }

    std::error_code error;
    fs::create_directories(work, error);
    (void)platform::write_file(log_path.string(), "");

    Failure failure;
    if (!output_writable(output, work / "output.json", failure)) {
        return fail(failure, {});
    }
    log::info("export step", {{"step", "validate"}});
    const fs::path data = fs::absolute(work / "data", error);
    if (!stage_data(project_dir, project.value(), data, failure)) {
        return fail(failure, log_path);
    }

    const std::string config = options.debug ? "Debug" : "Release";
    const fs::path build = work / "build";
    std::vector<std::string> configure{"cmake",
                                       "-S",
                                       (engine.value() / "export").string(),
                                       "-B",
                                       build.string(),
                                       "-DCMAKE_BUILD_TYPE=" + config,
                                       "-DSLASH_GAME_NAME=" + project.value().name,
                                       "-DSLASH_GAME_BINARY=" + project.value().binary_name(),
                                       "-DSLASH_GAME_ID=" + project.value().bundle_id(),
                                       "-DSLASH_GAME_VERSION=" + project.value().version,
                                       "-DSLASH_GAME_DATA=" + data.generic_string()};
    if (!fs::exists(build / "CMakeCache.txt", error) && program_runs("ninja")) {
        configure.insert(configure.begin() + 1, {"-G", "Ninja"});
    }
    if (options.platform == "macos") {
        configure.push_back("-DCMAKE_OSX_DEPLOYMENT_TARGET=" + std::string(kMacosDeploymentTarget));
        // Release builds run on Apple silicon and Intel Macs; debug builds only on this one.
        if (!options.debug) {
            configure.emplace_back("-DCMAKE_OSX_ARCHITECTURES=arm64;x86_64");
        }
    }
    if (toolchain) {
        configure.push_back("-DCMAKE_TOOLCHAIN_FILE=" + toolchain->generic_string());
    }

    platform::ProcessOptions process;
    process.log_path = log_path.string();
    if (environment("CPM_SOURCE_CACHE").empty()) {
        const fs::path engine_cache = engine.value() / ".cpm-cache";
        process.environment.emplace_back(
            "CPM_SOURCE_CACHE",
            fs::exists(engine_cache, error)
                ? engine_cache.string()
                : fs::absolute(work / ".." / "cpm-cache", error).lexically_normal().string());
    }

    const fs::path package = work / "package";
    fs::remove_all(package, error);
    if (!step("configure", configure, process, failure) ||
        !step("build", {"cmake", "--build", build.string(), "--config", config, "--parallel"},
              process, failure) ||
        !step("install",
              {"cmake", "--install", build.string(), "--config", config, "--component", "game",
               "--prefix", fs::absolute(package, error).string()},
              process, failure) ||
        !publish(package, output, work / "output.json", failure)) {
        return fail(failure, log_path);
    }

    const std::string binary = project.value().binary_name();
    fs::path executable;
    fs::path program;
    if (options.platform == "macos") {
        executable = output / (binary + ".app");
        program = executable / "Contents" / "MacOS" / binary;
    } else if (options.platform == "windows") {
        executable = output / (binary + ".exe");
        program = executable;
    } else if (options.platform == "linux") {
        executable = output / binary;
        program = executable;
    } else {
        executable = output / "index.html";
        program = output / (binary + ".js");
    }

    ExportResult result;
    result.ok = true;
    json::Value& out = result.json;
    out["platform"] = options.platform;
    out["output"] = display(output);
    out["executable"] = display(executable);
    out["bytes"] = folder_bytes(output);
    out["log"] = display(log_path);
    if (options.test) {
        json::Value test =
            run_test(options, fs::absolute(output, error), fs::absolute(program, error), log_path);
        if (const json::Value* passed = test.find("passed");
            passed != nullptr && !passed->as_bool()) {
            result.ok = false;
        }
        out["test"] = std::move(test);
    }
    const std::chrono::duration<double> elapsed = std::chrono::steady_clock::now() - started;
    out["seconds"] = static_cast<double>(static_cast<std::int64_t>(elapsed.count() * 10.0)) / 10.0;
    return result;
}

} // namespace slash::cli
