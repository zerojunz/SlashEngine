#pragma once

#include "slash/core/json.hpp"
#include "slash/core/result.hpp"

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// Unreal Engine projects, read through the artist's own Unreal Editor: its assets are binary
// packages that only Unreal reads, so the editor runs a Python script (unreal_export.py) that
// writes the project's levels as JSON and its meshes and sounds as glTF and WAV files, which the
// importer then converts. The editor runs as a program of its own, never linked, as Blender does.
namespace slash::importer::unreal {

// The .uproject file in a folder, if there is one.
[[nodiscard]] std::optional<std::filesystem::path>
find_project(const std::filesystem::path& folder);

// An Unreal Editor that can run the export: its command-line program (UnrealEditor-Cmd) and the
// engine version its folder names ("5.4"), when it names one.
struct Editor {
    std::string program;
    std::string version;
};

// The editor to export a project with: given when not empty, else SLASH_UNREAL_EDITOR, else one
// the Epic Games Launcher installed (its LauncherInstalled.dat, then the default install folders),
// preferring the version the project's EngineAssociation names, else the newest. Unreal Engine 5
// only.
[[nodiscard]] Result<Editor> find_editor(const std::filesystem::path& project,
                                         const std::string& given);

// The script the editor runs, as it is written into the work folder.
[[nodiscard]] std::string_view export_script();

// The arguments that run the export script on a project with an editor.
[[nodiscard]] std::vector<std::string> export_command(const Editor& editor,
                                                      const std::filesystem::path& project,
                                                      const std::filesystem::path& script);

// Runs the export with the editor into work (emptied first): writes the script there, runs the
// editor with SLASH_UNREAL_EXPORT naming work and its log in work/unreal.log, and returns
// export.json as it reads. Fails with the end of the log when the editor wrote no export. A
// request, when it is an object, goes to the script as work/request.json: its "properties" name
// the properties to read of each C++ class's actors and components, by class path.
[[nodiscard]] Result<json::Value> run_export(const Editor& editor,
                                             const std::filesystem::path& project,
                                             const std::filesystem::path& work,
                                             const json::Value& request = {});

// An Unreal config file (.ini): its sections by name, each key's values in order, after the
// +Key (add), -Key (remove), .Key (add even if there) and !Key (clear) lines are applied.
struct Config {
    struct Entry {
        std::string key;
        std::vector<std::string> values;
    };
    struct Section {
        std::string name;
        std::vector<Entry> entries;
    };
    std::vector<Section> sections;

    // The last value of a key in a section, or "" with none.
    [[nodiscard]] std::string value(std::string_view section, std::string_view key) const;
    // Every value of a key in a section.
    [[nodiscard]] std::vector<std::string> values(std::string_view section,
                                                  std::string_view key) const;
};
[[nodiscard]] Config parse_config(std::string_view text);

// The fields of an Unreal struct value as config files write it,
// (ActionName="Jump",bShift=False,Key=SpaceBar), unquoted; nested values are kept as text.
[[nodiscard]] std::vector<std::pair<std::string, std::string>> parse_struct(std::string_view value);

} // namespace slash::importer::unreal
