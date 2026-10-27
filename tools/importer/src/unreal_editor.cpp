#include "common.hpp"

#include "slash/importer/unreal.hpp"
#include "slash/platform/file.hpp"
#include "slash/platform/process.hpp"

#include <algorithm>
#include <cstddef>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace slash::importer {
namespace detail {
// unreal_export.py, built in by cmake/embed.cmake.
extern const unsigned char kUnrealExportPy[];
extern const std::size_t kUnrealExportPy_size;
} // namespace detail

namespace unreal {
namespace {

namespace fs = std::filesystem;

std::string trim(std::string_view text) {
    const auto first = text.find_first_not_of(" \t\r\n");
    if (first == std::string_view::npos) {
        return {};
    }
    const auto last = text.find_last_not_of(" \t\r\n");
    return std::string(text.substr(first, last - first + 1));
}

bool same_name(std::string_view a, std::string_view b) {
    return lowercase(a) == lowercase(b);
}

// The editor program inside an engine folder (the one holding Engine/), if it is there.
std::optional<std::string> editor_in(const fs::path& engine) {
    std::error_code error;
#if defined(_WIN32)
    const fs::path program = engine / "Engine/Binaries/Win64/UnrealEditor-Cmd.exe";
#elif defined(__APPLE__)
    const fs::path program =
        engine / "Engine/Binaries/Mac/UnrealEditor.app/Contents/MacOS/UnrealEditor";
#else
    fs::path program = engine / "Engine/Binaries/Linux/UnrealEditor-Cmd";
    if (!fs::exists(program, error)) {
        program = engine / "Engine/Binaries/Linux/UnrealEditor";
    }
#endif
    if (fs::is_regular_file(program, error)) {
        return program.generic_string();
    }
    return std::nullopt;
}

// "5.4" from an install folder named "UE_5.4", or "".
std::string version_of(const fs::path& folder) {
    const std::string name = folder.filename().generic_string();
    return name.starts_with("UE_") ? name.substr(3) : std::string();
}

// Orders "5.10" after "5.4".
bool newer(std::string_view a, std::string_view b) {
    const std::vector<std::string_view> left = split_path(a, '.');
    const std::vector<std::string_view> right = split_path(b, '.');
    for (std::size_t i = 0; i < std::max(left.size(), right.size()); ++i) {
        const std::int64_t x = i < left.size() ? parse_int(left[i]).value_or(0) : 0;
        const std::int64_t y = i < right.size() ? parse_int(right[i]).value_or(0) : 0;
        if (x != y) {
            return x > y;
        }
    }
    return false;
}

std::string home() {
#if defined(_WIN32)
    return platform::environment_variable("USERPROFILE");
#else
    return platform::environment_variable("HOME");
#endif
}

// The engine folders the Epic Games Launcher installed, from its LauncherInstalled.dat.
std::vector<fs::path> launcher_installs() {
#if defined(_WIN32)
    const std::string list = "C:/ProgramData/Epic/UnrealEngineLauncher/LauncherInstalled.dat";
#elif defined(__APPLE__)
    const std::string list =
        home() + "/Library/Application Support/Epic/UnrealEngineLauncher/LauncherInstalled.dat";
#else
    const std::string list = home() + "/.config/Epic/UnrealEngineLauncher/LauncherInstalled.dat";
#endif
    std::vector<fs::path> out;
    auto text = platform::read_file(list);
    auto parsed = text ? json::parse(text.value()) : Result<json::Value>(text.error());
    const json::Value* installs =
        parsed && parsed.value().is_object() ? parsed.value().find("InstallationList") : nullptr;
    if (installs == nullptr || !installs->is_array()) {
        return out;
    }
    for (const json::Value& install : installs->as_array()) {
        const json::Value* app = install.is_object() ? install.find("AppName") : nullptr;
        const json::Value* location =
            install.is_object() ? install.find("InstallLocation") : nullptr;
        if (app != nullptr && app->is_string() && app->as_string().starts_with("UE_") &&
            location != nullptr && location->is_string()) {
            out.emplace_back(location->as_string());
        }
    }
    return out;
}

// The folders the launcher puts engines in by default.
std::vector<fs::path> default_installs() {
#if defined(_WIN32)
    const fs::path root = "C:/Program Files/Epic Games";
#elif defined(__APPLE__)
    const fs::path root = "/Users/Shared/Epic Games";
#else
    const fs::path root = fs::path(home()) / "Epic Games";
#endif
    std::vector<fs::path> out;
    std::error_code error;
    for (fs::directory_iterator it(root, error), end; !error && it != end; it.increment(error)) {
        if (!version_of(it->path()).empty()) {
            out.push_back(it->path());
        }
    }
    return out;
}

// A source build registered under a GUID, as the project's EngineAssociation names it: Install.ini
// on macOS and Linux (Windows keeps them in the registry, which SLASH_UNREAL_EDITOR stands in for).
std::optional<fs::path> registered_build(std::string_view association) {
#if defined(__APPLE__)
    const std::string ini = home() + "/Library/Application Support/Epic/UnrealEngine/Install.ini";
#elif defined(_WIN32)
    const std::string ini;
#else
    const std::string ini = home() + "/.config/Epic/UnrealEngine/Install.ini";
#endif
    if (ini.empty()) {
        return std::nullopt;
    }
    auto text = platform::read_file(ini);
    if (!text) {
        return std::nullopt;
    }
    const Config config = parse_config(text.value());
    const std::string path = config.value("Installations", association);
    if (path.empty()) {
        return std::nullopt;
    }
    return fs::path(path);
}

std::string association_of(const fs::path& project) {
    auto text = platform::read_file(project.string());
    auto parsed = text ? json::parse(text.value()) : Result<json::Value>(text.error());
    const json::Value* association =
        parsed && parsed.value().is_object() ? parsed.value().find("EngineAssociation") : nullptr;
    return association != nullptr && association->is_string() ? association->as_string() : "";
}

Result<Editor> given_editor(const std::string& given, std::string_view from) {
    std::error_code error;
    if (fs::is_directory(given, error)) {
        if (auto program = editor_in(given)) {
            return Editor{.program = *program, .version = version_of(given)};
        }
        return Error{.code = ErrorCode::NotFound,
                     .message = std::string(from) + " names " + given +
                                ", a folder without Unreal Editor's program in Engine/Binaries"};
    }
    if (!fs::exists(given, error)) {
        return Error{.code = ErrorCode::NotFound,
                     .message = std::string(from) + " names " + given + ", which doesn't exist"};
    }
    // .../UE_5.4/Engine/Binaries/<Platform>/UnrealEditor-Cmd: the version from the engine folder.
    std::string version;
    for (fs::path folder = fs::path(given).parent_path(); folder.has_relative_path();
         folder = folder.parent_path()) {
        if (version = version_of(folder); !version.empty()) {
            break;
        }
    }
    return Editor{.program = given, .version = version};
}

std::string unquote(std::string_view value) {
    std::string text = trim(value);
    if (text.size() >= 2 && text.front() == '"' && text.back() == '"') {
        std::string out;
        for (std::size_t i = 1; i + 1 < text.size(); ++i) {
            if (text[i] == '\\' && i + 2 < text.size()) {
                ++i;
            }
            out += text[i];
        }
        return out;
    }
    return text;
}

} // namespace

std::optional<fs::path> find_project(const fs::path& folder) {
    std::error_code error;
    std::vector<fs::path> found;
    for (fs::directory_iterator it(folder, error), end; !error && it != end; it.increment(error)) {
        if (it->is_regular_file(error) &&
            lowercase(extension_of(it->path().generic_string())) == ".uproject") {
            found.push_back(it->path());
        }
    }
    std::ranges::sort(found);
    return found.empty() ? std::nullopt : std::optional<fs::path>(found.front());
}

Result<Editor> find_editor(const fs::path& project, const std::string& given) {
    if (!given.empty()) {
        return given_editor(given, "the editor option");
    }
    if (const std::string named = platform::environment_variable("SLASH_UNREAL_EDITOR");
        !named.empty()) {
        return given_editor(named, "SLASH_UNREAL_EDITOR");
    }
    const std::string association = association_of(project);
    if (!association.empty() && association.front() == '{') {
        if (const auto build = registered_build(association)) {
            if (auto program = editor_in(*build)) {
                return Editor{.program = *program, .version = {}};
            }
        }
    }
    std::vector<fs::path> installs = launcher_installs();
    for (fs::path& folder : default_installs()) {
        if (!contains(installs, folder)) {
            installs.push_back(std::move(folder));
        }
    }
    Editor best; // the newest version found, if its program isn't empty
    for (const fs::path& folder : installs) {
        const std::string version = version_of(folder);
        if (!version.starts_with("5.")) {
            continue;
        }
        const auto program = editor_in(folder);
        if (!program) {
            continue;
        }
        if (version == association) {
            return Editor{.program = *program, .version = version};
        }
        if (best.program.empty() || newer(version, best.version)) {
            best = Editor{.program = *program, .version = version};
        }
    }
    if (!best.program.empty()) {
        return best;
    }
    return Error{.code = ErrorCode::NotFound,
                 .message = "no Unreal Editor found: importing an Unreal project runs Unreal "
                            "Engine 5's editor, which reads its assets; install it with the Epic "
                            "Games Launcher or set SLASH_UNREAL_EDITOR to its UnrealEditor-Cmd"};
}

std::string_view export_script() {
    return {reinterpret_cast<const char*>(detail::kUnrealExportPy), detail::kUnrealExportPy_size};
}

std::vector<std::string> export_command(const Editor& editor, const fs::path& project,
                                        const fs::path& script) {
    return {editor.program, project.generic_string(), "-run=pythonscript",
            "-script=" + script.generic_string(), "-EnablePlugins=PythonScriptPlugin,GLTFExporter",
            // Commandlets start no renderer otherwise, and the glTF exporter bakes materials
            // with one.
            "-AllowCommandletRendering", "-unattended", "-nosplash", "-nopause", "-nosound",
            "-stdout", "-FullStdOutLogOutput"};
}

Result<json::Value> run_export(const Editor& editor, const fs::path& project, const fs::path& work,
                               const json::Value& request) {
    std::error_code error;
    fs::remove_all(work, error);
    if (auto made = platform::create_directories(work.string()); !made) {
        return made.error();
    }
    if (request.is_object()) {
        const fs::path asked = work / "request.json";
        if (auto written = platform::write_file(asked.string(), json::write(request) + "\n");
            !written) {
            return written.error();
        }
    }
    const fs::path script = work / "unreal_export.py";
    if (auto written = platform::write_file(script.string(), export_script()); !written) {
        return written.error();
    }
    const fs::path log = work / "unreal.log";
    auto run =
        platform::run_process(export_command(editor, project, script),
                              {.working_directory = {},
                               .environment = {{"SLASH_UNREAL_EXPORT", work.generic_string()}},
                               .log_path = log.string(),
                               .tail_lines = 30,
                               .input = {}});
    if (!run) {
        return Error{.code = ErrorCode::PlatformFailure,
                     .message = "Unreal Editor (" + editor.program +
                                ") doesn't run: " + run.error().message};
    }
    auto text = platform::read_file((work / "export.json").string());
    if (!text) {
        std::string why;
        if (auto failure = platform::read_file((work / "error.txt").string()); failure) {
            why = trim(failure.value());
            if (const auto line = why.rfind('\n'); line != std::string::npos) {
                why = why.substr(line + 1);
            }
            why = "the export script failed: " + why;
        } else {
            why = "it exited with " + std::to_string(run.value().exit_code) +
                  " without running the export script; the end of its log:\n" + run.value().tail;
        }
        return Error{.code = ErrorCode::PlatformFailure,
                     .message = "Unreal Editor wrote no export of " + project.generic_string() +
                                ": " + why};
    }
    auto parsed = json::parse(text.value());
    if (!parsed || !parsed.value().is_object()) {
        return Error{.code = ErrorCode::PlatformFailure,
                     .message = "Unreal Editor's export.json isn't a JSON object"};
    }
    const json::Value* format = parsed.value().find("format");
    if (format == nullptr || !format->is_number() || format->as_double() != 1.0) {
        return Error{.code = ErrorCode::PlatformFailure,
                     .message = "Unreal Editor's export.json is of a format this importer "
                                "doesn't read"};
    }
    return std::move(parsed.value());
}

std::string Config::value(std::string_view section, std::string_view key) const {
    const std::vector<std::string> all = values(section, key);
    return all.empty() ? std::string() : all.back();
}

std::vector<std::string> Config::values(std::string_view section, std::string_view key) const {
    for (const Section& in : sections) {
        if (!same_name(in.name, section)) {
            continue;
        }
        for (const Entry& entry : in.entries) {
            if (same_name(entry.key, key)) {
                return entry.values;
            }
        }
    }
    return {};
}

Config parse_config(std::string_view text) {
    Config config;
    Config::Section* section = nullptr;
    std::size_t at = 0;
    while (at < text.size()) {
        std::size_t end = text.find('\n', at);
        if (end == std::string_view::npos) {
            end = text.size();
        }
        const std::string line = trim(text.substr(at, end - at));
        at = end + 1;
        if (line.empty() || line.front() == ';' || line.front() == '#') {
            continue;
        }
        if (line.front() == '[' && line.back() == ']') {
            const std::string name = line.substr(1, line.size() - 2);
            const auto found = std::ranges::find_if(
                config.sections, [&](const Config::Section& s) { return s.name == name; });
            section =
                found != config.sections.end()
                    ? &*found
                    : &config.sections.emplace_back(Config::Section{.name = name, .entries = {}});
            continue;
        }
        if (section == nullptr) {
            continue;
        }
        char op = ' ';
        std::string_view rest = line;
        if (rest.front() == '+' || rest.front() == '-' || rest.front() == '.' ||
            rest.front() == '!') {
            op = rest.front();
            rest.remove_prefix(1);
        }
        const std::size_t equals = rest.find('=');
        const std::string key = trim(rest.substr(0, equals));
        const std::string value =
            equals == std::string_view::npos ? std::string() : unquote(rest.substr(equals + 1));
        auto found = std::ranges::find_if(section->entries, [&](const Config::Entry& entry) {
            return same_name(entry.key, key);
        });
        if (found == section->entries.end()) {
            found = section->entries.insert(section->entries.end(),
                                            Config::Entry{.key = key, .values = {}});
        }
        std::vector<std::string>& values = found->values;
        switch (op) {
        case '+':
            if (!contains(values, value)) {
                values.push_back(value);
            }
            break;
        case '.':
            values.push_back(value);
            break;
        case '-':
            std::erase(values, value);
            break;
        case '!':
            values.clear();
            break;
        default:
            values = {value};
        }
    }
    return config;
}

std::vector<std::pair<std::string, std::string>> parse_struct(std::string_view value) {
    std::vector<std::pair<std::string, std::string>> out;
    const std::string trimmed = trim(value);
    std::string_view text = trimmed;
    if (text.size() >= 2 && text.front() == '(' && text.back() == ')') {
        text = text.substr(1, text.size() - 2);
    }
    std::size_t start = 0;
    int depth = 0;
    bool quoted = false;
    for (std::size_t i = 0; i <= text.size(); ++i) {
        const char c = i < text.size() ? text[i] : ',';
        if (c == '"' && (i == 0 || text[i - 1] != '\\')) {
            quoted = !quoted;
        } else if (!quoted && c == '(') {
            ++depth;
        } else if (!quoted && c == ')') {
            --depth;
        } else if (!quoted && depth == 0 && c == ',') {
            const std::string_view field = text.substr(start, i - start);
            const std::size_t equals = field.find('=');
            if (equals != std::string_view::npos) {
                out.emplace_back(trim(field.substr(0, equals)), unquote(field.substr(equals + 1)));
            }
            start = i + 1;
        }
    }
    return out;
}

} // namespace unreal
} // namespace slash::importer
