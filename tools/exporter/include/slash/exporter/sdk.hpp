#pragma once

#include "slash/core/json.hpp"
#include "slash/core/json_schema.hpp"
#include "slash/core/result.hpp"
#include "slash/exporter/presets.hpp"

#include <filesystem>
#include <map>
#include <string>
#include <string_view>
#include <vector>

// SDK packages: a closed vendor SDK (an ad network, DLSS, Steamworks, a store or analytics
// service) added to a game without touching the engine, as Godot's Android and iOS plugins are.
// A package is a folder in the project with an sdk.json file that names the vendor's binaries
// for each platform:
//
//   {
//     "format": 1,
//     "name": "hello",
//     "version": "2.1.0",
//     "license": "Proprietary",
//     "downloads": {
//       "vendor": {"url": "https://example.com/hello-2.1.0.zip", "sha256": "...",
//                  "folder": "hello-sdk"}
//     },
//     "keys": {"HELLO_APP_ID": {"description": "The game's ID on the Hello dashboard."}},
//     "platforms": {
//       "linux": {"include": ["{vendor}/include"],
//                 "libraries": ["{vendor}/lib/linux-{arch}/libhello.so"],
//                 "defines": ["HELLO_APP_ID=\"${HELLO_APP_ID}\""]}
//     }
//   }
//
// project.json's "sdks" lists the packages' folders. Exports fetch each download once into
// sdks_dir(), checked against its SHA-256, build a package's own CMake project with the game's
// compilers, and link the packages whose platforms include the export's. A shared library is
// shipped where the platform's loader finds it, and the game is compiled with SLASH_SDK_<NAME>
// defined. Secrets such as API keys are named environment variables, as signing secrets are.
// Nothing from an SDK enters the engine's repository.
namespace slash::exporter {

inline constexpr std::string_view kSdkFile = "sdk.json";
inline constexpr std::int64_t kSdkFormat = 1;
// Where downloads are kept unless SLASH_SDKS_DIR says otherwise: ~/.slash/sdks.
inline constexpr std::string_view kSdksDirVariable = "SLASH_SDKS_DIR";

// A file or folder of the vendor's: downloaded and checked, or found on this machine.
struct SdkDownload {
    std::string id;
    std::string url;    // https:// (or file://) address; empty for a local path
    std::string sha256; // of what url returns
    std::string folder; // the folder inside the unpacked archive to use; "" for its root
    bool unpack = true; // unpack the archive; false keeps the file as it is (a .jar, a .dll)
    std::string path;   // a local file or folder, with ${VARIABLE}s from the environment
};

// An environment variable the package reads at export time, as ${NAME} in its defines,
// Info.plist and entitlement entries and Android manifest elements.
struct SdkKey {
    std::string name;
    std::string description;
    bool optional = false; // missing means "" instead of a failed export
};

// What a package adds to one platform's build. Paths are relative to the package's folder and
// may start with {sdk} (the folder), {build} (where its CMake project installed), a download's
// {id}, and contain {arch} (see sdk_arch).
struct SdkTarget {
    std::vector<std::string> include;
    std::vector<std::string> defines; // NAME or NAME=value
    // Static libraries, shared libraries and Windows import libraries to link. Shared ones are
    // shipped where the loader finds them.
    std::vector<std::string> libraries;
    std::vector<std::string> frameworks;        // Apple .framework and .xcframework folders
    std::vector<std::string> system_libraries;  // e.g. "ws2_32", "dl"
    std::vector<std::string> system_frameworks; // Apple, e.g. "StoreKit"
    std::vector<std::string> link_options;      // e.g. "-ObjC"
    std::vector<std::string> files;             // shipped beside the game
    json::Value plist;                          // Info.plist entries (macOS and iOS)
    json::Value entitlements;                   // code signing entitlements (macOS and iOS)
    // Android only.
    std::vector<std::string> permissions; // e.g. "android.permission.INTERNET"
    std::vector<std::string> manifest;    // XML elements added to <manifest>
    std::vector<std::string> application; // XML elements added to <application>
    std::vector<std::string> jars;        // Java libraries compiled against and packaged
};

struct Sdk {
    std::filesystem::path folder; // absolute
    std::string location;         // the folder as project.json names it
    std::string name;
    std::string version;
    std::string vendor;
    std::string description;
    std::string license;      // an SPDX identifier, or "Proprietary"
    std::string license_file; // a path as targets' paths are; "" for none
    std::vector<SdkDownload> downloads;
    std::string build;                                // a CMake project in the package; ""
    std::map<std::string, std::string> build_options; // -D options for it
    std::vector<SdkKey> keys;
    std::map<std::string, SdkTarget, std::less<>> platforms;

    // SLASH_SDK_ and the name in capitals with '_' for anything else: SLASH_SDK_HELLO.
    [[nodiscard]] std::string define() const;
};

// A problem with a package, at a JSON Pointer in its sdk.json.
struct SdkProblem {
    std::string sdk;  // the package's folder as project.json names it
    std::string path; // JSON Pointer into sdk.json; "" for the file itself
    std::string message;
};

// The JSON Schema for sdk.json.
[[nodiscard]] json::Value sdk_schema();

// Checks a parsed sdk.json against the schema and what it can't express: names, placeholders,
// keys and fields used on platforms that don't have them. Returns every error.
[[nodiscard]] std::vector<json::ValidationError> validate_sdk(const json::Value& sdk);

// Reads <folder>/sdk.json. The error names the first problem with its JSON Pointer.
[[nodiscard]] Result<Sdk> load_sdk(const std::filesystem::path& folder);

// The packages project.json's "sdks" names, in its order, and a problem for each one that
// doesn't load or repeats another's name.
[[nodiscard]] std::vector<Sdk> load_project_sdks(const std::filesystem::path& project_dir,
                                                 const std::vector<std::string>& locations,
                                                 std::vector<SdkProblem>& problems);

// The packages an export with preset links: those its "sdks" names, or else every package
// with a section for its platform. A name the preset lists that no package has is a problem.
[[nodiscard]] std::vector<const Sdk*> sdks_for_preset(const std::vector<Sdk>& sdks,
                                                      const Preset& preset,
                                                      std::vector<SdkProblem>& problems);

// What {arch} means in an export for platform: the Linux architecture, the Android ABI,
// "x86_64" on Windows, "universal" on macOS, "device" or "simulator" on iOS and "wasm32" on the
// web.
[[nodiscard]] std::vector<std::string> sdk_archs(const Preset& preset);

// SLASH_SDKS_DIR, or ~/.slash/sdks.
[[nodiscard]] std::filesystem::path sdks_dir();

// Where a download is, once fetched or found: the unpacked folder, the file, or the local path.
// Not checked to exist.
[[nodiscard]] std::filesystem::path download_location(const SdkDownload& download,
                                                      const std::filesystem::path& sdk_folder,
                                                      const std::filesystem::path& dir);

// Downloads with curl into dir, checks the SHA-256 and unpacks with CMake (zip, tar.gz, tar.xz
// and the rest), unless it is there already. A local path is only checked to exist. Returns
// download_location. Download output is appended to log_path.
[[nodiscard]] Result<std::filesystem::path> fetch_download(const SdkDownload& download,
                                                           const std::filesystem::path& sdk_folder,
                                                           const std::filesystem::path& dir,
                                                           const std::string& log_path = {});

// A target's paths and texts with their placeholders and ${KEY}s filled in, for one {arch}.
struct ResolvedSdk {
    const Sdk* sdk = nullptr;
    std::string platform;
    std::string arch;
    std::vector<std::string> include;
    std::vector<std::string> defines;
    std::vector<std::string> libraries;
    std::vector<std::string> frameworks;
    std::vector<std::string> system_libraries;
    std::vector<std::string> system_frameworks;
    std::vector<std::string> link_options;
    std::vector<std::string> files;
    json::Value plist;
    json::Value entitlements;
    std::vector<std::string> permissions;
    std::vector<std::string> manifest;
    std::vector<std::string> application;
    std::vector<std::string> jars;
    std::string license_file; // absolute; "" for none
};

// Where a package's pieces are on this machine, for resolve_sdk.
struct SdkPlaces {
    std::map<std::string, std::filesystem::path> downloads; // by id
    std::filesystem::path build;                            // its CMake project's install
    std::map<std::string, std::string> keys;                // values by name
};

// Fills in sdk's target for platform and arch. A path that doesn't exist, unless check_paths is
// false, is a problem; so is a {build} path when the package has no CMake project.
[[nodiscard]] ResolvedSdk resolve_sdk(const Sdk& sdk, std::string_view platform,
                                      std::string_view arch, const SdkPlaces& places,
                                      bool check_paths, std::vector<SdkProblem>& problems);

// The keys' values from the environment; a required key that isn't set is a problem.
[[nodiscard]] std::map<std::string, std::string> sdk_keys(const Sdk& sdk,
                                                          std::vector<SdkProblem>& problems);

// The packages' entries in one of their objects ("plist" or "entitlements") merged by key. Two
// packages that set one key to different values are a problem.
[[nodiscard]] json::Value merge_sdk_entries(const std::vector<ResolvedSdk>& sdks,
                                            std::string_view field,
                                            std::vector<SdkProblem>& problems);

// The CMake that export/CMakeLists.txt includes to link packages into the game: one
// slash_game_sdk() call per package, then the Info.plist entries (plist, merged) and the
// Android manifest elements.
[[nodiscard]] std::string sdk_cmake(const std::vector<ResolvedSdk>& sdks, const json::Value& plist);

// Info.plist entries as the XML between <dict> and </dict>, keys in order.
[[nodiscard]] std::string plist_entries(const json::Value& entries);

// A plist with these entries, for entitlements.
[[nodiscard]] std::string plist_document(const json::Value& entries);

// A plist file as JSON, read with plutil (macOS only).
[[nodiscard]] Result<json::Value> read_plist(const std::filesystem::path& path);

// Checks the packages of the project in folder: their sdk.json files, the presets' "sdks", the
// keys in the environment (unless keys is false), and that each file a platform's builds name
// exists (for platform only, unless it is ""). Downloads not fetched yet are fetched if fetch,
// else listed as "unchecked". Paths in {build} are checked when an export builds the package.
// Returns {"sdks": [...], "problems": [{"sdk", "path", "message"}]}.
[[nodiscard]] json::Value check_sdks(const std::filesystem::path& project,
                                     std::string_view platform, bool fetch, bool keys = true,
                                     const std::string& log_path = {});

struct AddSdkOptions {
    std::filesystem::path project;
    // A package's folder; a git repository (ending in .git, or git@ or ssh://); or an https://
    // address of a .zip or .tar archive with sdk.json at its root or in its one folder.
    std::string source;
    std::string name;   // the folder under sdks/; "" means the package's name
    std::string ref;    // a git branch or tag; "" for the default branch
    std::string sha256; // an archive's expected SHA-256; "" to not check
    bool dry_run = false;
};

// Adds a package to project.json's "sdks": a folder inside the project where it is, anything
// else copied into sdks/<name>. Returns {"name", "version", "license", "folder", "copied",
// "platforms", "keys"}.
[[nodiscard]] Result<json::Value> add_sdk(const AddSdkOptions& options);

// Whether a library file is shared (ELF shared object, Mach-O dylib, PE DLL) rather than a static
// archive; for .framework folders, whether their binary is.
[[nodiscard]] bool is_shared_library(const std::filesystem::path& path);

} // namespace slash::exporter
