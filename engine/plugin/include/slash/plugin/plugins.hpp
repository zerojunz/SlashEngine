#pragma once

#include "slash/core/json.hpp"
#include "slash/core/json_schema.hpp"
#include "slash/core/result.hpp"
#include "slash/plugin/slash_plugin.h"
#include "slash/world/component_registry.hpp"
#include "slash/world/world.hpp"

#include <cstdint>
#include <map>
#include <span>
#include <string>
#include <string_view>
#include <vector>

// Plugins, as Godot's GDExtension: libraries built apart from the engine against
// slash/plugin/slash_plugin.h, a C interface that stays the same from one engine version to the
// next, which add components and systems to a game. A project lists them in project.json's
// "plugins", each a .plugin.json file naming its entry function and its library per platform.
//
//   plugin::Plugins plugins(components);
//   plugins.load_project(root, project.plugins);  // before scenes open
//   plugins.attach(world);                        // each world gets the plugins' systems
namespace slash::plugin {

// A system a plugin registered.
struct PluginSystem {
    std::string name;
    std::string doc;
    std::int32_t priority = 0;
    void (*run)(void* userdata, SlashWorld* world, double tick_seconds) = nullptr;
    void* userdata = nullptr;
};

// A .plugin.json file, as Godot's .gdextension: the entry function's name, the interface version
// it was built for, and its library on each platform as "<os>.<arch>" or "<os>" ("linux.x86_64",
// "macos", "windows.x86_64", "android.arm64", "ios", "web"), relative to the file.
struct PluginFile {
    std::string path; // the file, as the project names it
    std::string entry;
    std::uint32_t abi = SLASH_PLUGIN_ABI_VERSION;
    std::map<std::string, std::string, std::less<>> libraries;
};

// $cli schema plugin.
[[nodiscard]] json::Value plugin_file_schema();
// Schema errors, then a library key that names no platform, and an abi newer than the engine's.
[[nodiscard]] std::vector<json::ValidationError> validate_plugin_file(const json::Value& file);
[[nodiscard]] Result<PluginFile> parse_plugin_file(const json::Value& file, std::string path);

// The library a file names for a platform ("<os>.<arch>", then "<os>"), as a path relative to
// the project, or "" if it names none.
[[nodiscard]] std::string library_for(const PluginFile& file, std::string_view os,
                                      std::string_view arch);
// This program's CPU, as library keys name it: "x86_64", "arm64" or "wasm32".
[[nodiscard]] std::string_view cpu_name();

// The function get_proc gives a plugin for a name: what a plugin's tests or a host other than
// Plugins can hand an entry function.
[[nodiscard]] SlashProc get_proc(const char* name);

// The plugins a program loaded, and the systems they add to its worlds. Plugins stay loaded until
// the program exits, as Godot keeps extensions; their components stay in the registry, which
// must outlive this object.
class Plugins {
public:
    explicit Plugins(world::ComponentRegistry& components);
    Plugins(const Plugins&) = delete;
    Plugins& operator=(const Plugins&) = delete;
    Plugins(Plugins&&) = delete;
    Plugins& operator=(Plugins&&) = delete;
    // Calls each plugin's deinitialize, after taking its systems out of the worlds.
    ~Plugins();

    // Loads a plugin's library and calls its entry function, which registers its components and
    // systems. A plugin that registers anything wrong, or refuses, adds nothing.
    Result<void> load(const std::string& library, const std::string& entry, std::string name);
    // The same for a plugin linked into the program (iOS and web exports).
    Result<void> load_linked(SlashPluginEntry entry, std::string name);
    // Loads the plugin files a project lists, for this platform, and returns each one's error
    // (also logged), by file.
    std::map<std::string, std::string, std::less<>>
    load_project(const std::string& root, std::span<const std::string> files);

    // Adds the plugins' systems to a world, and to plugins loaded later; detach takes them out.
    void attach(world::World& world);
    void detach(world::World& world);

    // [{"name", "components": [...], "systems": [...]}] for each loaded plugin.
    [[nodiscard]] json::Value status() const;
    [[nodiscard]] std::size_t size() const noexcept { return loaded_.size(); }

private:
    struct Loaded {
        std::string name;
        std::vector<std::string> components;
        std::vector<PluginSystem> systems;
        SlashPluginInit init{};
    };

    Result<void> enter(SlashPluginEntry entry, std::string name);
    void add_systems(world::World& world, const Loaded& plugin) const;

    world::ComponentRegistry* components_;
    std::vector<Loaded> loaded_;
    std::vector<world::World*> worlds_;
};

} // namespace slash::plugin
