#pragma once

#include "slash/core/json.hpp"
#include "slash/core/json_schema.hpp"
#include "slash/core/result.hpp"

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// Export presets live in export_presets.json at the project root, like Godot's
// export_presets.cfg. Each preset names a platform and how to build and sign for it:
//
//   {
//     "format": 1,
//     "presets": {
//       "mac-release": {
//         "platform": "macos",
//         "macos": {"codesign_identity": "Developer ID Application: Example (ABCDE12345)",
//                   "notarize": true}
//       }
//     }
//   }
//
// Secrets never go in the file. Passwords and keys come from environment variables, listed in
// the schema's descriptions, so the file can be committed and edited by agents.
namespace slash::exporter {

inline constexpr std::string_view kPresetsFile = "export_presets.json";
inline constexpr std::int64_t kPresetsFormat = 1;

// The platforms export knows, in the order usage text lists them.
inline const std::vector<std::string_view> kPlatforms{"macos", "windows", "linux",
                                                      "web",   "android", "ios"};

// Android CPU architectures, and the ones an export builds unless a preset says otherwise:
// phones and tablets, plus x86_64 for emulators and Chromebooks.
inline const std::vector<std::string_view> kAndroidAbis{"arm64-v8a", "armeabi-v7a", "x86_64",
                                                        "x86"};
inline const std::vector<std::string> kDefaultAndroidAbis{"arm64-v8a", "x86_64"};

// Environment variables that hold signing secrets.
inline constexpr std::string_view kAppleApiKeyPath = "SLASH_APPLE_API_KEY_PATH";
inline constexpr std::string_view kAppleApiKeyId = "SLASH_APPLE_API_KEY_ID";
inline constexpr std::string_view kAppleApiIssuer = "SLASH_APPLE_API_ISSUER";
inline constexpr std::string_view kWindowsCertificatePath = "SLASH_WINDOWS_CERTIFICATE_PATH";
inline constexpr std::string_view kWindowsCertificatePassword =
    "SLASH_WINDOWS_CERTIFICATE_PASSWORD";
inline constexpr std::string_view kAndroidKeystorePath = "SLASH_ANDROID_KEYSTORE_PATH";
inline constexpr std::string_view kAndroidKeystorePassword = "SLASH_ANDROID_KEYSTORE_PASSWORD";
inline constexpr std::string_view kAndroidKeyAlias = "SLASH_ANDROID_KEY_ALIAS";
inline constexpr std::string_view kAndroidKeyPassword = "SLASH_ANDROID_KEY_PASSWORD";
inline constexpr std::string_view kIosProvisioningProfile = "SLASH_IOS_PROVISIONING_PROFILE";

// Where an iOS export runs: on iPhones and iPads, signed for a team, or in the Simulator.
inline constexpr std::string_view kIosDevice = "device";
inline constexpr std::string_view kIosSimulator = "simulator";

inline constexpr std::string_view kAdHocIdentity = "-";
inline constexpr std::string_view kDefaultTimestampUrl = "http://timestamp.digicert.com";

struct MacosOptions {
    // codesign identity from the keychain; "-" signs ad hoc, which runs only on the Mac that
    // built the game or after the player allows it.
    std::string codesign_identity = std::string(kAdHocIdentity);
    std::string entitlements; // project-relative .plist; optional
    bool notarize = false;    // submit to Apple's notary service and staple the ticket
};

struct WindowsOptions {
    bool sign = false; // sign with the certificate in SLASH_WINDOWS_CERTIFICATE_PATH
    std::string timestamp_url = std::string(kDefaultTimestampUrl); // "" for no timestamp
};

// The CPU architectures Linux exports build for.
inline constexpr std::string_view kLinuxX86_64 = "x86_64";
inline constexpr std::string_view kLinuxArm64 = "arm64";

struct LinuxOptions {
    std::string arch = std::string(kLinuxX86_64);
};

struct AndroidOptions {
    std::vector<std::string> abis = kDefaultAndroidAbis;
    std::string package;           // "" means the project's id
    std::int64_t version_code = 0; // 0 means derived from the project's version
    // The adaptive icon's layers (slash/exporter/icons.hpp): a project-relative image for the
    // foreground, "" for the icon; a "#rrggbb" color or an image for the background.
    std::string icon_foreground;
    std::string icon_background = "#ffffff";
};

struct IosOptions {
    std::string team_id; // Apple Developer team; device builds are signed for it
    // kIosDevice or kIosSimulator. Without a team, presets build for the Simulator.
    std::string destination = std::string(kIosSimulator);
    std::string bundle_id; // "" means the project's id
    // "" means the team's "Apple Development" or "Apple Distribution" identity; "-" signs ad hoc.
    std::string codesign_identity;
    std::int64_t build_number = 0;           // CFBundleVersion; 0 means the project's version
    std::string icon_background = "#ffffff"; // behind the icon's transparent pixels
};

struct Preset {
    std::string name;
    std::string platform;
    std::string output; // project-relative; "" means build/export/<name>
    bool debug = false;
    std::string icon; // project-relative; "" means project.json's
    // The quality tier its model recipes ship as (slash/scene3d/recipe.hpp); "" means as written.
    std::string model_quality;
    // The SDK packages it links, by name; nullopt means every one with a section for the
    // platform (slash/exporter/sdk.hpp).
    std::optional<std::vector<std::string>> sdks;
    MacosOptions macos;
    WindowsOptions windows;
    AndroidOptions android;
    LinuxOptions linux_options; // "linux" is a macro on some compilers
    IosOptions ios;
};

using Presets = std::map<std::string, Preset, std::less<>>;

// The JSON Schema for export_presets.json.
[[nodiscard]] json::Value presets_schema();

// Checks a parsed export_presets.json against the schema and the rules it can't express:
// preset names, project-relative paths, platform-specific sections and signing settings.
// Returns every error.
[[nodiscard]] std::vector<json::ValidationError> validate_presets(const json::Value& presets);

// Reads validated presets. The error names the first problem with its JSON Pointer.
[[nodiscard]] Result<Presets> parse_presets(const json::Value& presets);

// Reads <folder>/export_presets.json. A project without the file has no presets.
[[nodiscard]] Result<Presets> load_presets(std::string_view folder);

// Why name can't be an Android package name, or "" if it can: two or more dot-separated parts,
// each a Java identifier of ASCII letters, digits and '_' that starts with a letter and isn't a
// Java keyword.
[[nodiscard]] std::string android_package_problem(std::string_view name);

// Why id can't be an iOS bundle ID, or "" if it can: two or more dot-separated parts of ASCII
// letters, digits and '-'.
[[nodiscard]] std::string ios_bundle_id_problem(std::string_view id);

// The Android versionCode for a major.minor.patch version: major * 1000000 + minor * 1000 +
// patch, so every release sorts after the one before. The error says when the version doesn't
// fit (minor or patch over 999, or a code over Google Play's 2100000000).
[[nodiscard]] Result<std::int64_t> android_version_code(std::string_view version);

// The preset called target, or else the defaults for the platform called target. The error
// suggests the closest preset or platform name.
[[nodiscard]] Result<Preset> find_preset(const Presets& presets, std::string_view target);

} // namespace slash::exporter
