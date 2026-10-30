#pragma once

#include "slash/runtime/project.hpp"

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

// How exports ship a project's plugins (slash/plugin/plugins.hpp), as Godot's export ships each
// GDExtension's library for the platform:
//
//   - A plugin file with "sources" is compiled into the game, on every platform, and entered
//     through the slash_game_plugins function the export writes.
//   - Otherwise its library for the platform ships: in the data folder at the path its file
//     names on Windows and Linux, in the app bundle's Frameworks on macOS and in the APK's
//     lib/<abi>/ on Android, where games look for it by its file name. iOS and the web load no
//     libraries, so there it is a static library (.a) linked into the game.
//
// The plugin files themselves ship in the data folder, as the game reads them.
namespace slash::exporter {

// One plugin in one build of a game.
struct ShippedPlugin {
    std::string file;  // the plugin file, as project.json lists it
    std::string name;  // its file's name before the first dot
    std::string entry; // the entry function
    // Its sources, absolute, when it is compiled in.
    std::vector<std::filesystem::path> sources;
    // Its library for the build, absolute, and that library's path in the project.
    std::filesystem::path library;
    std::string library_path;
    // Whether the game enters it linked in (sources or a static library), not by loading.
    [[nodiscard]] bool linked() const { return !sources.empty() || library_path.ends_with(".a"); }
};

// A problem with a plugin file: the file, the JSON Pointer of the bad value in it, and why.
struct PluginProblem {
    std::string file;
    std::string path;
    std::string message;
};

// The plugins of a project for one build: platform as presets name it and arch as SDK packages'
// {arch} does ("x86_64", "arm64", an Android ABI, "universal" on macOS, the iOS destination,
// "wasm32"). Problems are by file, with JSON Pointer paths into it.
[[nodiscard]] std::vector<ShippedPlugin> plan_plugins(const std::filesystem::path& project_dir,
                                                      const runtime::Project& project,
                                                      std::string_view platform,
                                                      std::string_view arch,
                                                      std::vector<PluginProblem>& problems);

// The plugin key's CPU for a build's arch: "arm64" for "arm64-v8a", "" where the platform's key
// has none (macOS's universal libraries, iOS).
[[nodiscard]] std::string_view plugin_arch(std::string_view platform, std::string_view arch);

// The C++ file defining slash_game_plugins(), which names the linked plugins' entry functions to
// the game; "" if none is linked.
[[nodiscard]] std::string plugins_register_source(const std::vector<ShippedPlugin>& plugins);

// The slash_game_plugin() calls export/CMakeLists.txt includes for a build, and
// slash_game_plugins_register() for the file plugins_register_source wrote, if any.
[[nodiscard]] std::string plugins_cmake(const std::vector<ShippedPlugin>& plugins,
                                        const std::string& register_file);

} // namespace slash::exporter
