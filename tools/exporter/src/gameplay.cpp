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
#include <array>
#include <charconv>
#include <cstdint>
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

enum class Severity : std::uint8_t { Error, Warning, Note };

// Reads "file:line:column: error: message" (GCC and Clang; also warnings and notes) or
// "file(line,column): error C1234: message" (MSVC) from one line of output.
bool read_diagnostic(std::string_view line, const std::string& project, CompileError& out,
                     Severity& severity) {
    struct Marker {
        std::string_view text;
        Severity severity;
    };
    constexpr std::array<Marker, 4> kMarkers{{{": fatal error: ", Severity::Error},
                                              {": error: ", Severity::Error},
                                              {": warning: ", Severity::Warning},
                                              {": note: ", Severity::Note}}};
    std::size_t at = std::string_view::npos;
    std::size_t marker_size = 0;
    for (const Marker& marker : kMarkers) {
        const std::size_t found = line.find(marker.text);
        if (found < at) {
            at = found;
            marker_size = marker.text.size();
            severity = marker.severity;
        }
    }
    if (at != std::string_view::npos) {
        std::string_view where = line.substr(0, at);
        out.message = std::string(line.substr(at + marker_size));
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
    severity = Severity::Error;
    at = line.find("): error ");
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

// Reads a quoted string as -fdiagnostics-parseable-fixits writes it (\\, \", \t, \n and
// three octal digits escaped) from the start of text, past its closing quote.
bool read_quoted(std::string_view& text, std::string& out) {
    if (!text.starts_with('"')) {
        return false;
    }
    std::size_t i = 1;
    while (i < text.size() && text[i] != '"') {
        if (text[i] != '\\' || i + 1 >= text.size()) {
            out += text[i++];
            continue;
        }
        const char next = text[i + 1];
        if (next >= '0' && next <= '7' && i + 3 < text.size()) {
            int value = 0;
            for (std::size_t j = i + 1; j < i + 4; ++j) {
                value = value * 8 + (text[j] - '0');
            }
            out += static_cast<char>(value);
            i += 4;
            continue;
        }
        if (next == 'n') {
            out += '\n';
        } else if (next == 't') {
            out += '\t';
        } else {
            out += next;
        }
        i += 2;
    }
    if (i >= text.size()) {
        return false;
    }
    text.remove_prefix(i + 1);
    return true;
}

// Reads a number from the start of text and the character that must follow it.
bool read_number(std::string_view& text, char then, int& out) {
    const std::size_t end = text.find(then);
    if (end == std::string_view::npos || !parse_int(text.substr(0, end), out)) {
        return false;
    }
    text.remove_prefix(end + 1);
    return true;
}

// Reads a line of -fdiagnostics-parseable-fixits: fix-it:"file":{1:2-3:4}:"text".
bool read_fix(std::string_view line, const std::string& project, CompileFix& out) {
    constexpr std::string_view kPrefix = "fix-it:";
    if (!line.starts_with(kPrefix)) {
        return false;
    }
    line.remove_prefix(kPrefix.size());
    std::string file;
    if (!read_quoted(line, file) || !line.starts_with(":{")) {
        return false;
    }
    line.remove_prefix(2);
    if (!read_number(line, ':', out.line) || !read_number(line, '-', out.column) ||
        !read_number(line, ':', out.end_line) || !read_number(line, '}', out.end_column) ||
        !line.starts_with(':')) {
        return false;
    }
    line.remove_prefix(1);
    out.file = project_file(file, project);
    return read_quoted(line, out.text);
}

json::Value diagnostics_json(const std::vector<CompileError>& errors) {
    json::Array out;
    for (const CompileError& error : errors) {
        json::Value item;
        item["file"] = error.file;
        item["line"] = error.line;
        if (error.column > 0) {
            item["column"] = error.column;
        }
        item["message"] = error.message;
        if (!error.fix.empty()) {
            json::Array edits;
            for (const CompileFix& fix : error.fix) {
                json::Value edit;
                if (fix.file != error.file) {
                    edit["file"] = fix.file;
                }
                edit["line"] = fix.line;
                edit["column"] = fix.column;
                if (fix.end_line != fix.line) {
                    edit["end_line"] = fix.end_line;
                }
                edit["end_column"] = fix.end_column;
                edit["text"] = fix.text;
                edits.push_back(std::move(edit));
            }
            item["fix"] = std::move(edits);
        }
        if (!error.notes.empty()) {
            json::Array notes;
            for (const CompileNote& note : error.notes) {
                json::Value entry;
                entry["file"] = note.file;
                entry["line"] = note.line;
                if (note.column > 0) {
                    entry["column"] = note.column;
                }
                entry["message"] = note.message;
                notes.push_back(std::move(entry));
            }
            item["notes"] = std::move(notes);
        }
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
            out["errors"] = diagnostics_json(errors);
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

CompileDiagnostics read_compile_diagnostics(std::string_view log, const std::string& project,
                                            std::size_t limit) {
    CompileDiagnostics out;
    CompileError* current = nullptr; // the error or warning notes and fixes belong to
    bool in_note = false;            // a note's fixes are alternatives, not the compiler's fix
    while (!log.empty()) {
        const std::size_t end = log.find('\n');
        std::string_view line = log.substr(0, end);
        log = end == std::string_view::npos ? std::string_view() : log.substr(end + 1);
        if (line.ends_with('\r')) {
            line.remove_suffix(1);
        }
        if (CompileFix fix; read_fix(line, project, fix)) {
            if (current != nullptr && !in_note) {
                current->fix.push_back(std::move(fix));
            }
            continue;
        }
        CompileError found;
        Severity severity = Severity::Error;
        if (!read_diagnostic(line, project, found, severity)) {
            continue; // the source line and caret under it, and the like
        }
        if (severity == Severity::Note) {
            in_note = true;
            if (current != nullptr && current->notes.size() < kMaxNotes) {
                current->notes.push_back({.file = std::move(found.file),
                                          .line = found.line,
                                          .column = found.column,
                                          .message = std::move(found.message)});
            }
            continue;
        }
        in_note = false;
        current = nullptr;
        std::vector<CompileError>& list = severity == Severity::Error ? out.errors : out.warnings;
        if (list.size() >= limit || std::ranges::any_of(list, [&](const CompileError& seen) {
                return seen.file == found.file && seen.line == found.line &&
                       seen.column == found.column && seen.message == found.message;
            })) {
            continue;
        }
        list.push_back(std::move(found));
        current = &list.back();
    }
    return out;
}

std::vector<CompileError> read_compile_errors(std::string_view log, const std::string& project,
                                              std::size_t limit) {
    return read_compile_diagnostics(log, project, limit).errors;
}

namespace {

// Why a step of building or checking a game's code failed.
struct Failure {
    std::string step;
    std::string message;
    std::string tail; // the end of the step's output
};

// A project's configured gameplay build.
struct Workspace {
    std::string root;  // the project, absolute
    fs::path build;    // <project>/.slash/gameplay/build
    fs::path log;      // where each step's output goes
    std::string label; // the log, relative to the project
};

// Runs one step, its output appended to the log; false with failure set if it fails.
bool run_step(const std::string& step, const std::vector<std::string>& args,
              const fs::path& log_path, Failure& failure) {
    log::info("building gameplay code", {{"step", step}});
    platform::ProcessOptions process;
    process.log_path = log_path.string();
    // The compiler's messages in English with ASCII quotes, whatever the user's locale, so they
    // read the same everywhere and errors and warnings are found by their English labels.
    process.environment = {{"LC_ALL", "C"}};
    auto result = platform::run_process(args, process);
    if (!result) {
        failure.step = step;
        failure.message = result.error().message;
        return false;
    }
    if (result.value().exit_code != 0) {
        failure.step = step;
        failure.tail = result.value().tail;
        return false;
    }
    return true;
}

// Checks what building a project's code needs, then configures its build in
// <project>/.slash/gameplay/build with CMake, the output in <project>/.slash/gameplay/<log_name>.
std::optional<Workspace> configure_gameplay(const std::string& project, const std::string& engine,
                                            std::string_view log_name, Failure& failure) {
    const auto fail = [&](std::string message) {
        failure.step = "options";
        failure.message = std::move(message);
        return std::nullopt;
    };
    auto loaded = runtime::load_project(project);
    if (!loaded) {
        return fail(loaded.error().message);
    }
    const std::vector<std::string>& sources = loaded.value().gameplay_sources;
    if (sources.empty()) {
        return fail("project.json has no gameplay sources; add "
                    "\"gameplay\": {\"sources\": [\"src\"]}");
    }
    if (!platform::can_load_libraries()) {
        return fail("the tools load gameplay libraries on macOS and Linux only; exports for every "
                    "platform link the code in");
    }
    std::error_code error;
    const fs::path sdk = SLASH_GAMEPLAY_SDK;
    if (sdk.empty() || !fs::exists(sdk, error)) {
        return fail("this program's engine build has no gameplay-sdk.cmake; build the engine "
                    "with CMake first");
    }
    const std::optional<std::string> engine_dir = find_engine(engine);
    if (!engine_dir) {
        return fail("could not find the engine sources; set SLASH_ENGINE_DIR to a SlashEngine "
                    "checkout");
    }
    if (!detail::program_runs("cmake")) {
        return fail("building gameplay code needs CMake 3.25 or newer on PATH");
    }

    Workspace out;
    const fs::path root = fs::absolute(project, error).lexically_normal();
    out.root = root.generic_string();
    if (out.root.size() > 1 && out.root.back() == '/') {
        out.root.pop_back(); // "game/." normalizes to "game/"
    }
    const fs::path work = root / ".slash" / "gameplay";
    out.build = work / "build";
    out.log = work / log_name;
    out.label = ".slash/gameplay/" + std::string(log_name);
    fs::create_directories(work, error);
    fs::remove(out.log, error);

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
                                       out.build.string(),
                                       "-C",
                                       sdk.string(),
                                       "-DSLASH_PROJECT_DIR=" + out.root,
                                       "-DSLASH_GAMEPLAY_SOURCES=" + source_list};
    if (!fs::exists(out.build / "CMakeCache.txt", error) &&
        !std::string_view(SLASH_GAMEPLAY_GENERATOR).empty()) {
        configure.insert(configure.begin() + 1, {"-G", SLASH_GAMEPLAY_GENERATOR});
    }
    if (!run_step("configure", configure, out.log, failure)) {
        if (failure.message.empty()) {
            failure.message = "CMake could not configure the gameplay build; see the log";
        }
        return std::nullopt;
    }
    return out;
}

// The command building a configured gameplay build's target, going on past files that fail so
// every file's errors are found.
std::vector<std::string> build_command(const fs::path& build, std::string_view target) {
    std::vector<std::string> out{"cmake", "--build", build.string()};
    if (!target.empty()) {
        out.insert(out.end(), {"--target", std::string(target)});
    }
    const auto cache = platform::read_file((build / "CMakeCache.txt").string());
    const std::string_view text = cache ? std::string_view(cache.value()) : std::string_view();
    constexpr std::string_view kGenerator = "CMAKE_GENERATOR:INTERNAL=";
    const std::size_t at = text.find(kGenerator);
    const std::string_view generator =
        at == std::string_view::npos
            ? std::string_view()
            : text.substr(at + kGenerator.size(), text.find('\n', at) - at - kGenerator.size());
    if (generator.starts_with("Ninja")) {
        out.insert(out.end(), {"--", "-k", "0"});
    } else if (generator.ends_with("Makefiles")) {
        out.insert(out.end(), {"--", "-k"});
    }
    return out;
}

// The source files a build compiled, from the objects its log says it built ("Building CXX
// object <object>", as Ninja and Make print it) and the build's compile_commands.json.
std::vector<std::string> compiled_files(std::string_view log, const Workspace& workspace) {
    std::vector<std::pair<std::string, std::string>> objects; // object, source
    const auto commands = platform::read_file((workspace.build / "compile_commands.json").string());
    if (auto parsed = json::parse(commands ? commands.value() : "[]");
        parsed && parsed.value().is_array()) {
        for (const json::Value& entry : parsed.value().as_array()) {
            const json::Value* output = entry.find("output");
            const json::Value* file = entry.find("file");
            if (output != nullptr && output->is_string() && file != nullptr && file->is_string()) {
                objects.emplace_back(output->as_string(), file->as_string());
            }
        }
    }
    std::vector<std::string> out;
    constexpr std::string_view kBuilding = "Building CXX object ";
    for (std::size_t at = log.find(kBuilding); at != std::string_view::npos;
         at = log.find(kBuilding, at + 1)) {
        std::string_view object = log.substr(at + kBuilding.size());
        object = object.substr(0, object.find_first_of("\r\n"));
        const auto found = std::ranges::find_if(objects, [&](const auto& entry) {
            return entry.first == object || object.ends_with("/" + entry.first) ||
                   entry.first.ends_with("/" + std::string(object));
        });
        if (found != objects.end()) {
            out.push_back(project_file(found->second, workspace.root));
        }
    }
    std::ranges::sort(out);
    out.erase(std::ranges::unique(out).begin(), out.end());
    return out;
}

} // namespace

GameplayBuild build_gameplay(const std::string& project, const std::string& engine) {
    GameplayBuild out;
    Failure failure;
    const std::optional<Workspace> workspace =
        configure_gameplay(project, engine, "build.log", failure);
    if (failure.step != "options") {
        out.log = ".slash/gameplay/build.log";
    }
    if (workspace &&
        run_step("build", build_command(workspace->build, ""), workspace->log, failure)) {
        out.ok = true;
        out.library = gameplay::library_path("");
        return out;
    }
    out.step = failure.step;
    out.message = failure.message;
    out.tail = failure.tail;
    if (workspace && out.message.empty()) { // the build ran and failed
        const auto text = platform::read_file(workspace->log.string());
        out.errors = read_compile_errors(text ? text.value() : std::string(), workspace->root);
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

json::Value GameplayCheck::to_json() const {
    json::Value out;
    if (ran) {
        out["ok"] = diagnostics.errors.empty();
        json::Array files;
        for (const std::string& file : checked) {
            files.emplace_back(file);
        }
        out["checked"] = std::move(files);
        if (!diagnostics.errors.empty()) {
            out["errors"] = diagnostics_json(diagnostics.errors);
        }
        if (!diagnostics.warnings.empty()) {
            out["warnings"] = diagnostics_json(diagnostics.warnings);
        }
        return out;
    }
    out["step"] = step;
    if (!tail.empty()) {
        out["tail"] = tail;
    }
    if (!log.empty()) {
        out["log"] = log;
    }
    return out;
}

GameplayCheck check_gameplay(const std::string& project, const std::string& engine) {
    GameplayCheck out;
    Failure failure;
    const std::optional<Workspace> workspace =
        configure_gameplay(project, engine, "check.log", failure);
    if (workspace) {
        // The objects alone: compiled where their sources changed, with nothing linked.
        const bool built = run_step("build", build_command(workspace->build, "game_objects"),
                                    workspace->log, failure);
        if (built || failure.message.empty()) {
            const auto text = platform::read_file(workspace->log.string());
            const std::string_view output = text ? std::string_view(text.value()) : "";
            out.diagnostics = read_compile_diagnostics(output, workspace->root);
            out.checked = compiled_files(output, *workspace);
            if (built || !out.diagnostics.errors.empty()) {
                out.ran = true;
                return out;
            }
            failure.message = "the gameplay build failed with no compiler errors; see the log";
        }
    }
    out.step = failure.step;
    out.message = failure.message;
    out.tail = failure.tail;
    if (failure.step != "options") {
        out.log = ".slash/gameplay/check.log";
    }
    return out;
}

} // namespace slash::exporter
