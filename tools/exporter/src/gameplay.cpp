#include "slash/exporter/gameplay.hpp"

#include "steps.hpp"

#include "slash/core/log.hpp"
#include "slash/exporter/export.hpp"
#include "slash/gameplay/host.hpp"
#include "slash/platform/file.hpp"
#include "slash/platform/library.hpp"
#include "slash/platform/process.hpp"
#include "slash/runtime/project.hpp"

#include <algorithm>
#include <charconv>
#include <filesystem>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#ifndef SLASH_GAMEPLAY_SDK
#define SLASH_GAMEPLAY_SDK ""
#endif
#ifndef SLASH_GAMEPLAY_GENERATOR
#define SLASH_GAMEPLAY_GENERATOR ""
#endif

namespace slash::exporter {
namespace {

namespace fs = std::filesystem;

bool parse_int(std::string_view text, int& out) {
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), out);
    return error == std::errc() && end == text.data() + text.size() && !text.empty();
}

// A file the compiler named, relative to the project when inside it.
std::string project_file(std::string_view file, const std::string& project) {
    std::string path(file);
    std::ranges::replace(path, '\\', '/');
    if (!project.empty() && path.starts_with(project) && path.size() > project.size() &&
        path[project.size()] == '/') {
        return path.substr(project.size() + 1);
    }
    return path;
}

// Reads "file:line:column: error: message" (GCC and Clang) or
// "file(line,column): error C1234: message" (MSVC) from one line of output.
bool read_error(std::string_view line, const std::string& project, CompileError& out) {
    for (const std::string_view marker :
         {std::string_view(": fatal error: "), std::string_view(": error: ")}) {
        const std::size_t at = line.find(marker);
        if (at == std::string_view::npos) {
            continue;
        }
        std::string_view where = line.substr(0, at);
        out.message = std::string(line.substr(at + marker.size()));
        // file:line:column, or file:line.
        const std::size_t last = where.rfind(':');
        if (last == std::string_view::npos) {
            return false;
        }
        int number = 0;
        if (!parse_int(where.substr(last + 1), number)) {
            return false;
        }
        where = where.substr(0, last);
        const std::size_t before = where.rfind(':');
        int line_number = 0;
        if (before != std::string_view::npos && parse_int(where.substr(before + 1), line_number)) {
            out.line = line_number;
            out.column = number;
            where = where.substr(0, before);
        } else {
            out.line = number;
            out.column = 0;
        }
        out.file = project_file(where, project);
        return !out.file.empty();
    }
    // MSVC: file(line,column): error C2065: message
    const std::size_t at = line.find("): error ");
    const std::size_t open = at == std::string_view::npos ? at : line.rfind('(', at);
    if (open == std::string_view::npos) {
        return false;
    }
    const std::string_view numbers = line.substr(open + 1, at - open - 1);
    const std::size_t comma = numbers.find(',');
    if (!parse_int(numbers.substr(0, comma), out.line)) {
        return false;
    }
    out.column = 0;
    if (comma != std::string_view::npos && !parse_int(numbers.substr(comma + 1), out.column)) {
        return false;
    }
    std::string_view message = line.substr(at + 9);
    if (const std::size_t colon = message.find(": "); colon != std::string_view::npos) {
        message.remove_prefix(colon + 2);
    }
    out.message = std::string(message);
    out.file = project_file(line.substr(0, open), project);
    return !out.file.empty();
}

json::Value errors_json(const std::vector<CompileError>& errors) {
    json::Array out;
    for (const CompileError& error : errors) {
        json::Value item;
        item["file"] = error.file;
        item["line"] = error.line;
        if (error.column > 0) {
            item["column"] = error.column;
        }
        item["message"] = error.message;
        out.push_back(std::move(item));
    }
    return out;
}

} // namespace

json::Value GameplayBuild::to_json() const {
    json::Value out;
    if (ok) {
        out["library"] = library;
    } else {
        out["step"] = step;
        if (!errors.empty()) {
            out["errors"] = errors_json(errors);
        }
        if (!tail.empty()) {
            out["tail"] = tail;
        }
    }
    if (!log.empty()) {
        out["log"] = log;
    }
    return out;
}

std::vector<CompileError> read_compile_errors(std::string_view log, const std::string& project,
                                              std::size_t limit) {
    std::vector<CompileError> out;
    while (!log.empty() && out.size() < limit) {
        const std::size_t end = log.find('\n');
        std::string_view line = log.substr(0, end);
        log = end == std::string_view::npos ? std::string_view() : log.substr(end + 1);
        if (line.ends_with('\r')) {
            line.remove_suffix(1);
        }
        CompileError error;
        if (read_error(line, project, error) &&
            std::ranges::none_of(out, [&](const CompileError& seen) {
                return seen.file == error.file && seen.line == error.line &&
                       seen.column == error.column && seen.message == error.message;
            })) {
            out.push_back(std::move(error));
        }
    }
    return out;
}

GameplayBuild build_gameplay(const std::string& project, const std::string& engine) {
    GameplayBuild out;
    const auto fail = [&](std::string step, std::string message) {
        out.step = std::move(step);
        out.message = std::move(message);
        return out;
    };
    auto loaded = runtime::load_project(project);
    if (!loaded) {
        return fail("options", loaded.error().message);
    }
    const std::vector<std::string>& sources = loaded.value().gameplay_sources;
    if (sources.empty()) {
        return fail("options", "project.json has no gameplay sources; add "
                               "\"gameplay\": {\"sources\": [\"src\"]}");
    }
    if (!platform::can_load_libraries()) {
        return fail("options", "the tools load gameplay libraries on macOS and Linux only; "
                               "exports for every platform link the code in");
    }
    std::error_code error;
    const fs::path sdk = SLASH_GAMEPLAY_SDK;
    if (sdk.empty() || !fs::exists(sdk, error)) {
        return fail("options", "this program's engine build has no gameplay-sdk.cmake; build the "
                               "engine with CMake first");
    }
    const std::optional<std::string> engine_dir = find_engine(engine);
    if (!engine_dir) {
        return fail("options", "could not find the engine sources; set SLASH_ENGINE_DIR to a "
                               "SlashEngine checkout");
    }
    if (!detail::program_runs("cmake")) {
        return fail("options", "building gameplay code needs CMake 3.25 or newer on PATH");
    }

    const fs::path root = fs::absolute(project, error).lexically_normal();
    std::string root_text = root.generic_string();
    if (root_text.size() > 1 && root_text.back() == '/') {
        root_text.pop_back(); // "game/." normalizes to "game/"
    }
    const fs::path work = root / ".slash" / "gameplay";
    const fs::path build = work / "build";
    const fs::path log_path = work / "build.log";
    fs::create_directories(work, error);
    fs::remove(log_path, error);
    out.log = ".slash/gameplay/build.log";

    std::string source_list;
    for (const std::string& source : sources) {
        if (!source_list.empty()) {
            source_list += ';';
        }
        source_list += (root / source).lexically_normal().generic_string();
    }
    std::vector<std::string> configure{"cmake",
                                       "-S",
                                       (fs::path(*engine_dir) / "cmake" / "gameplay").string(),
                                       "-B",
                                       build.string(),
                                       "-C",
                                       sdk.string(),
                                       "-DSLASH_PROJECT_DIR=" + root_text,
                                       "-DSLASH_GAMEPLAY_SOURCES=" + source_list};
    if (!fs::exists(build / "CMakeCache.txt", error) &&
        !std::string_view(SLASH_GAMEPLAY_GENERATOR).empty()) {
        configure.insert(configure.begin() + 1, {"-G", SLASH_GAMEPLAY_GENERATOR});
    }
    platform::ProcessOptions process;
    process.log_path = log_path.string();
    const auto run = [&](const std::string& step, const std::vector<std::string>& args) {
        log::info("building gameplay code", {{"step", step}});
        auto result = platform::run_process(args, process);
        if (!result) {
            fail(step, result.error().message);
            return false;
        }
        if (result.value().exit_code != 0) {
            out.step = step;
            out.tail = result.value().tail;
            return false;
        }
        return true;
    };
    if (!run("configure", configure)) {
        if (out.message.empty()) {
            out.message = "CMake could not configure the gameplay build; see the log";
        }
        return out;
    }
    if (!run("build", {"cmake", "--build", build.string()})) {
        if (out.message.empty()) {
            const auto text = platform::read_file(log_path.string());
            out.errors = read_compile_errors(text ? text.value() : std::string(), root_text);
            if (out.errors.empty()) {
                out.message = "the gameplay build failed; see the log";
            } else {
                out.tail.clear();
                out.message = "the game's code has " + std::to_string(out.errors.size()) +
                              (out.errors.size() == 1 ? " error" : " errors");
            }
        }
        return out;
    }
    out.ok = true;
    out.library = gameplay::library_path("");
    return out;
}

} // namespace slash::exporter
