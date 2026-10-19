#pragma once

#include "slash/core/json.hpp"
#include "slash/core/result.hpp"

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// A Unity player build turned back into a Unity project, for the Unity importer: the build is
// found and checked here, then AssetRipper (GPL 3.0, so only ever run as a separate program)
// rebuilds the project, which is tidied so it converts as a project made in Unity would.
namespace slash::importer::unity {

// A Unity build found at a path.
struct Build {
    // What AssetRipper loads: the build's folder, or an .apk or .ipa file.
    std::filesystem::path root;
    // The player data folder (Game_Data, Contents/Resources/Data, assets/bin/Data), or empty for
    // a web build or a package file.
    std::filesystem::path data;
    // A web build's data file, often compressed, and its other files.
    std::filesystem::path web_data;
    std::vector<std::filesystem::path> web_files;
    std::string platform; // windows, linux, macos, web, android or ios
    std::string backend;  // mono, il2cpp, or "" when the build doesn't say
    bool package = false; // an .apk or .ipa file
};

// The Unity build at a path, or nullopt if there is none: a folder with a Game_Data folder (or
// the data folder itself, or a file in the build such as its executable), a macOS .app, a web
// build's folder (with Build/*.data, compressed or not), an Android build's unpacked
// assets/bin/Data, or an .apk or .ipa file.
[[nodiscard]] std::optional<Build> find_build(const std::filesystem::path& source);

// The Unity version a build's file was made with, from a serialized file's or UnityFS bundle's
// header, or "" if it is neither.
[[nodiscard]] std::string file_unity_version(std::string_view header);

// Whether a UnityFS bundle's header marks it encrypted (UnityCN), which nothing here decrypts.
[[nodiscard]] bool encrypted_bundle(std::string_view header);

// A web build's data file decompressed: gzip, brotli (Unity's, which starts with a comment
// naming it) or none. Fails if it isn't Unity web data (UnityWebData1.0) once decompressed.
[[nodiscard]] Result<std::string> decompress_web_data(std::string_view bytes);

// One file in Unity web data.
struct WebFile {
    std::string name;
    std::string_view data;
};

// The files of Unity web data (UnityWebData1.0), viewing bytes.
[[nodiscard]] Result<std::vector<WebFile>> read_web_data(std::string_view bytes);

struct UnpackOptions {
    // The AssetRipper program; empty means SLASH_ASSETRIPPER, else the installed one, installed
    // first if need be.
    std::string assetripper;
    // An empty folder to work in: the staged web build, AssetRipper's export and its log.
    std::filesystem::path work;
    // How long AssetRipper may take to start, in seconds.
    int start_timeout = 120;
};

struct UnpackedBuild {
    // The Unity project AssetRipper rebuilt, with Assets/ and ProjectSettings/.
    std::filesystem::path project;
    // What came back: the platform, scripting backend, Unity version and AssetRipper version,
    // counts of scenes, scripts and assets, and "unsupported", what didn't.
    json::Value report;
};

// Rebuilds the build's Unity project with AssetRipper in options.work. Refuses builds with
// encrypted bundles. Afterwards, the scripts of Unity's own packages (uGUI, TextMesh Pro, the
// event system, URP's 2D lights) get the GUIDs they have in Unity's packages again, so their
// components convert, and their stand-in scripts are dropped.
[[nodiscard]] Result<UnpackedBuild> unpack_build(const Build& build, const UnpackOptions& options);

// The GUID a script of one of Unity's packages has in every project, by namespace and class
// name, or "" if the importer doesn't know it.
[[nodiscard]] std::string_view package_script_guid(std::string_view space, std::string_view name);

} // namespace slash::importer::unity
