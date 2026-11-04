#include "slash/exporter/plugins.hpp"

#include "slash/core/json.hpp"
#include "slash/platform/file.hpp"
#include "slash/platform/platform.hpp"
#include "slash/plugin/plugins.hpp"

#include <algorithm>
#include <array>
#include <optional>
#include <system_error>

namespace slash::exporter {
namespace fs = std::filesystem;
namespace {

constexpr std::array<std::string_view, 6> kSourceSuffixes{".c", ".cc", ".cpp", ".cxx", ".m", ".mm"};

bool is_source(const fs::path& file) {
    return std::ranges::find(kSourceSuffixes, file.extension().string()) != kSourceSuffixes.end();
}

// Objective-C builds only for Apple's platforms.
bool builds_for(const fs::path& file, std::string_view platform) {
    return (file.extension() != ".m" && file.extension() != ".mm") || platform == "macos" ||
           platform == "ios";
}

// A CMake bracket argument holding text as it is.
std::string bracket(std::string_view text) {
    std::string equals;
    while (text.find("]" + equals + "]") != std::string_view::npos) {
        equals += '=';
    }
    return "[" + equals + "[" + std::string(text) + "]" + equals + "]";
}

} // namespace

std::string_view plugin_arch(std::string_view platform, std::string_view arch) {
    if (platform == "macos" || platform == "ios") {
        return {};
    }
    if (arch == "arm64-v8a" || arch == "aarch64") {
        return "arm64";
    }
    return arch;
}

std::vector<std::string> plugins_to_check(const fs::path& project_dir,
                                          const runtime::Project& project,
                                          std::vector<PluginProblem>& unchecked) {
    std::vector<std::string> out;
    for (const std::string& path : project.plugins) {
        // A file that can't be read is loaded all the same, so loading reports it.
        std::optional<plugin::PluginFile> file;
        if (auto text = platform::read_file((project_dir / path).string())) {
            if (auto document = json::parse(text.value())) {
                if (auto parsed = plugin::parse_plugin_file(document.value(), path)) {
                    file = std::move(parsed.value());
                }
            }
        }
        if (!file) {
            out.push_back(path);
            continue;
        }
        const std::string os(platform::os_name());
        const std::string library = plugin::library_for(*file, os, plugin::cpu_name());
        std::error_code error;
        if (library.empty()) {
            unchecked.push_back(
                {path, "",
                 "it names no library for " + os + "." + std::string(plugin::cpu_name())});
        } else if (!file->sources.empty() && !fs::exists(project_dir / library, error)) {
            unchecked.push_back({path, "/libraries",
                                 "its library for this machine, \"" + library +
                                     "\", isn't built; exports compile its sources"});
        } else {
            out.push_back(path);
        }
    }
    return out;
}

std::vector<ShippedPlugin> plan_plugins(const fs::path& project_dir,
                                        const runtime::Project& project, std::string_view platform,
                                        std::string_view arch,
                                        std::vector<PluginProblem>& problems) {
    std::vector<ShippedPlugin> out;
    for (std::size_t i = 0; i < project.plugins.size(); ++i) {
        const std::string& path = project.plugins[i];
        const auto fail = [&](std::string at, std::string message) {
            problems.push_back({path, std::move(at), std::move(message)});
        };
        auto text = platform::read_file((project_dir / path).string());
        if (!text) {
            problems.push_back({std::string(runtime::kProjectFile), "/plugins/" + std::to_string(i),
                                "no file \"" + path + "\""});
            continue;
        }
        auto document = json::parse(text.value());
        if (!document) {
            fail("", document.error().message);
            continue;
        }
        const std::vector<json::ValidationError> errors =
            plugin::validate_plugin_file(document.value());
        if (!errors.empty()) {
            for (const json::ValidationError& error : errors) {
                fail(error.path, error.message);
            }
            continue;
        }
        const plugin::PluginFile file = plugin::parse_plugin_file(document.value(), path).value();
        ShippedPlugin shipped;
        shipped.file = path;
        const std::string name = fs::path(path).filename().string();
        shipped.name = name.substr(0, name.find('.'));
        shipped.entry = file.entry;
        const fs::path folder = (project_dir / path).parent_path();
        std::error_code error;
        for (std::size_t s = 0; s < file.sources.size(); ++s) {
            const fs::path source = (folder / file.sources[s]).lexically_normal();
            const std::string at = "/sources/" + std::to_string(s);
            if (fs::is_directory(source, error)) {
                std::vector<fs::path> found;
                for (auto it = fs::recursive_directory_iterator(source, error);
                     !error && it != fs::recursive_directory_iterator(); it.increment(error)) {
                    if (it->is_regular_file(error) && is_source(it->path()) &&
                        builds_for(it->path(), platform)) {
                        found.push_back(fs::absolute(it->path(), error));
                    }
                }
                if (found.empty() && fs::is_empty(source, error)) {
                    fail(at, "the folder \"" + file.sources[s] + "\" is empty");
                }
                std::ranges::sort(found);
                shipped.sources.insert(shipped.sources.end(), found.begin(), found.end());
            } else if (!fs::is_regular_file(source, error)) {
                fail(at, "no file \"" + file.sources[s] + "\"");
            } else if (!is_source(source)) {
                fail(at, "expected a .c, .cc, .cpp, .cxx, .m or .mm file");
            } else if (builds_for(source, platform)) {
                shipped.sources.push_back(fs::absolute(source, error));
            }
        }
        if (!file.sources.empty() && shipped.sources.empty()) {
            fail("/sources", "none of its sources builds for " + std::string(platform));
            continue;
        }
        if (file.sources.empty()) {
            shipped.library_path = plugin::library_for(file, platform, plugin_arch(platform, arch));
            if (shipped.library_path.empty()) {
                std::string key(platform);
                if (const std::string_view cpu = plugin_arch(platform, arch); !cpu.empty()) {
                    key += "." + std::string(cpu);
                }
                fail("/libraries", "it names no library for " + key + ", and no sources");
                continue;
            }
            shipped.library = fs::absolute(project_dir / shipped.library_path, error);
            const std::string library_name = shipped.library.filename().string();
            if (!fs::is_regular_file(shipped.library, error)) {
                fail("/libraries", "no file \"" + shipped.library_path + "\"");
                continue;
            }
            const bool static_library = library_name.ends_with(".a");
            if ((platform == "ios" || platform == "web") && !static_library) {
                fail("/libraries", "iOS and web games load no libraries: name a static library "
                                   "(.a) for " +
                                       std::string(platform) + ", or give sources");
                continue;
            }
            if (platform != "ios" && platform != "web" && static_library) {
                fail("/libraries", "expected a shared library for " + std::string(platform) +
                                       ": static ones link only into iOS and web games");
                continue;
            }
            if (platform == "android" &&
                (!library_name.starts_with("lib") || !library_name.ends_with(".so"))) {
                // Android installs only the libraries in an APK named lib<name>.so.
                fail("/libraries",
                     "Android libraries must be named lib<name>.so, not " + library_name);
                continue;
            }
        }
        out.push_back(std::move(shipped));
    }
    return out;
}

std::string plugins_register_source(const std::vector<ShippedPlugin>& plugins) {
    std::string declarations;
    std::string body;
    for (const ShippedPlugin& plugin : plugins) {
        if (!plugin.linked()) {
            continue;
        }
        declarations += "extern \"C\" int " + plugin.entry +
                        "(SlashGetProc get_proc, SlashPlugin* plugin, SlashPluginInit* init);\n";
        body += "    plugins.emplace(\"" + plugin.entry + "\", &" + plugin.entry + ");\n";
    }
    if (body.empty()) {
        return {};
    }
    return "// Written by slash-cli export: the plugins compiled or linked into this game.\n"
           "#include \"slash/plugin/slash_plugin.h\"\n\n"
           "#include <functional>\n#include <map>\n#include <string>\n\n" +
           declarations +
           "\nvoid slash_game_plugins(std::map<std::string, SlashPluginEntry, std::less<>>& "
           "plugins) {\n" +
           body + "}\n";
}

std::string plugins_cmake(const std::vector<ShippedPlugin>& plugins,
                          const std::string& register_file) {
    std::string out = "# Written by slash-cli export: the game's plugins for this build.\n";
    for (const ShippedPlugin& plugin : plugins) {
        out += "slash_game_plugin(" + bracket(plugin.name);
        if (!plugin.sources.empty()) {
            out += "\n    SOURCES";
            for (const fs::path& source : plugin.sources) {
                out += " " + bracket(source.generic_string());
            }
        } else if (plugin.linked()) {
            out += "\n    STATIC " + bracket(plugin.library.generic_string());
        } else {
            out += "\n    SHARED " + bracket(plugin.library.generic_string());
        }
        out += ")\n";
    }
    if (!register_file.empty()) {
        out += "slash_game_plugins_register(" + bracket(register_file) + ")\n";
    }
    return out;
}

} // namespace slash::exporter
